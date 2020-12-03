// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "olap/snapshot_manager.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>

#include <algorithm>
#include <boost/date_time/gregorian/gregorian.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/filesystem.hpp>
#include <iterator>
#include <map>
#include <set>

#include "env/env.h"
#include "gen_cpp/Types_constants.h"
#include "olap/olap_snapshot_converter.h"
#include "olap/rowset/alpha_rowset_meta.h"
#include "olap/rowset/rowset.h"
#include "olap/rowset/rowset_converter.h"
#include "olap/rowset/rowset_factory.h"
#include "olap/rowset/rowset_id_generator.h"
#include "olap/rowset/rowset_writer.h"
#include "olap/storage_engine.h"

using boost::filesystem::copy_file;
using boost::filesystem::copy_option;
using boost::filesystem::path;
using std::map;
using std::nothrow;
using std::set;
using std::string;
using std::stringstream;
using std::vector;
using std::list;

namespace doris {

SnapshotManager* SnapshotManager::_s_instance = nullptr;
std::mutex SnapshotManager::_mlock;

SnapshotManager* SnapshotManager::instance() {
    if (_s_instance == nullptr) {
        std::lock_guard<std::mutex> lock(_mlock);
        if (_s_instance == nullptr) {
            _s_instance = new SnapshotManager();
        }
    }
    return _s_instance;
}

OLAPStatus SnapshotManager::make_snapshot(const TSnapshotRequest& request, string* snapshot_path) {
    OLAPStatus res = OLAP_SUCCESS;
    if (snapshot_path == nullptr) {
        LOG(WARNING) << "output parameter cannot be NULL";
        return OLAP_ERR_INPUT_PARAMETER_ERROR;
    }

    TabletSharedPtr ref_tablet = StorageEngine::instance()->tablet_manager()->get_tablet(
            request.tablet_id, request.schema_hash);
    if (ref_tablet == nullptr) {
        LOG(WARNING) << "failed to get tablet. tablet=" << request.tablet_id
                     << " schema_hash=" << request.schema_hash;
        return OLAP_ERR_TABLE_NOT_FOUND;
    }

    res = _create_snapshot_files(ref_tablet, request, snapshot_path,
                                 request.preferred_snapshot_version);
    // if all nodes has been upgraded, it can be removed
    if (request.__isset.missing_version && res == OLAP_SUCCESS) {
        (const_cast<TSnapshotRequest&>(request)).__set_allow_incremental_clone(true);
    }

    if (res != OLAP_SUCCESS) {
        LOG(WARNING) << "failed to make snapshot. res=" << res << " tablet=" << request.tablet_id
                     << " schema_hash=" << request.schema_hash;
        return res;
    }

    LOG(INFO) << "success to make snapshot. path=['" << *snapshot_path << "']";
    return res;
}

OLAPStatus SnapshotManager::release_snapshot(const string& snapshot_path) {
    // 如果请求的snapshot_path位于root/snapshot文件夹下，则认为是合法的，可以删除
    // 否则认为是非法请求，返回错误结果
    auto stores = StorageEngine::instance()->get_stores();
    for (auto store : stores) {
        std::string abs_path;
        RETURN_WITH_WARN_IF_ERROR(FileUtils::canonicalize(store->path(), &abs_path),
                                  OLAP_ERR_DIR_NOT_EXIST,
                                  "canonical path " + store->path() + "failed");

        if (snapshot_path.compare(0, abs_path.size(), abs_path) == 0 &&
            snapshot_path.compare(abs_path.size(), SNAPSHOT_PREFIX.size(), SNAPSHOT_PREFIX) == 0) {
            FileUtils::remove_all(snapshot_path);
            LOG(INFO) << "success to release snapshot path. [path='" << snapshot_path << "']";

            return OLAP_SUCCESS;
        }
    }

    LOG(WARNING) << "released snapshot path illegal. [path='" << snapshot_path << "']";
    return OLAP_ERR_CE_CMD_PARAMS_ERROR;
}

// TODO support beta rowset
// For now, alpha and beta rowset meta have same fields, so we can just use
// AlphaRowsetMeta here.
OLAPStatus SnapshotManager::convert_rowset_ids(const string& clone_dir, int64_t tablet_id,
                                               const int32_t& schema_hash) {
    OLAPStatus res = OLAP_SUCCESS;
    // check clone dir existed
    if (!FileUtils::check_exist(clone_dir)) {
        res = OLAP_ERR_DIR_NOT_EXIST;
        LOG(WARNING) << "clone dir not existed when convert rowsetids. clone_dir=" << clone_dir;
        return res;
    }

    // load original tablet meta
    string cloned_meta_file = clone_dir + "/" + std::to_string(tablet_id) + ".hdr";
    TabletMeta cloned_tablet_meta;
    if ((res = cloned_tablet_meta.create_from_file(cloned_meta_file)) != OLAP_SUCCESS) {
        LOG(WARNING) << "fail to load original tablet meta after clone. "
                     << ", cloned_meta_file=" << cloned_meta_file;
        return res;
    }
    TabletMetaPB cloned_tablet_meta_pb;
    cloned_tablet_meta.to_meta_pb(&cloned_tablet_meta_pb);

    TabletMetaPB new_tablet_meta_pb;
    new_tablet_meta_pb = cloned_tablet_meta_pb;
    new_tablet_meta_pb.clear_rs_metas();
    new_tablet_meta_pb.clear_inc_rs_metas();
    // should modify tablet id and schema hash because in restore process the tablet id is not
    // equal to tablet id in meta
    new_tablet_meta_pb.set_tablet_id(tablet_id);
    new_tablet_meta_pb.set_schema_hash(schema_hash);
    TabletSchema tablet_schema;
    tablet_schema.init_from_pb(new_tablet_meta_pb.schema());

    std::unordered_map<Version, RowsetMetaPB*, HashOfVersion> _rs_version_map;
    for (auto& visible_rowset : cloned_tablet_meta_pb.rs_metas()) {
        RowsetMetaPB* rowset_meta = new_tablet_meta_pb.add_rs_metas();
        RowsetId rowset_id = StorageEngine::instance()->next_rowset_id();
        RETURN_NOT_OK(_rename_rowset_id(visible_rowset, clone_dir, tablet_schema, rowset_id,
                                        rowset_meta));
        rowset_meta->set_tablet_id(tablet_id);
        rowset_meta->set_tablet_schema_hash(schema_hash);
        Version rowset_version = {visible_rowset.start_version(), visible_rowset.end_version()};
        _rs_version_map[rowset_version] = rowset_meta;
    }

    for (auto& inc_rowset : cloned_tablet_meta_pb.inc_rs_metas()) {
        Version rowset_version = {inc_rowset.start_version(), inc_rowset.end_version()};
        auto exist_rs = _rs_version_map.find(rowset_version);
        if (exist_rs != _rs_version_map.end()) {
            RowsetMetaPB* rowset_meta = new_tablet_meta_pb.add_inc_rs_metas();
            *rowset_meta = *(exist_rs->second);
            continue;
        }
        RowsetMetaPB* rowset_meta = new_tablet_meta_pb.add_inc_rs_metas();
        RowsetId rowset_id = StorageEngine::instance()->next_rowset_id();
        RETURN_NOT_OK(
                _rename_rowset_id(inc_rowset, clone_dir, tablet_schema, rowset_id, rowset_meta));
        rowset_meta->set_tablet_id(tablet_id);
        rowset_meta->set_tablet_schema_hash(schema_hash);
    }

    res = TabletMeta::save(cloned_meta_file, new_tablet_meta_pb);
    if (res != OLAP_SUCCESS) {
        LOG(WARNING) << "fail to save converted tablet meta to dir='" << clone_dir;
        return res;
    }

    return OLAP_SUCCESS;
}

OLAPStatus SnapshotManager::_rename_rowset_id(const RowsetMetaPB& rs_meta_pb,
                                              const string& new_path, TabletSchema& tablet_schema,
                                              const RowsetId& rowset_id,
                                              RowsetMetaPB* new_rs_meta_pb) {
    OLAPStatus res = OLAP_SUCCESS;
    // TODO use factory to obtain RowsetMeta when SnapshotManager::convert_rowset_ids supports beta rowset
    // TODO(cmy): now we only has AlphaRowsetMeta, and no BetaRowsetMeta.
    //            AlphaRowsetMeta only add some functions about segment group, and no addition fields.
    //            So we can use AlphaRowsetMeta here even if this is a beta rowset.
    //            And the `rowset_type` field indicates the real type of rowset, so that the correct rowset
    //            can be created.
    RowsetMetaSharedPtr alpha_rowset_meta(new AlphaRowsetMeta());
    alpha_rowset_meta->init_from_pb(rs_meta_pb);
    RowsetSharedPtr org_rowset;
    RETURN_NOT_OK(
            RowsetFactory::create_rowset(&tablet_schema, new_path, alpha_rowset_meta, &org_rowset));
    // do not use cache to load index
    // because the index file may conflict
    // and the cached fd may be invalid
    RETURN_NOT_OK(org_rowset->load(false));
    RowsetMetaSharedPtr org_rowset_meta = org_rowset->rowset_meta();
    RowsetWriterContext context;
    context.rowset_id = rowset_id;
    context.tablet_id = org_rowset_meta->tablet_id();
    context.partition_id = org_rowset_meta->partition_id();
    context.tablet_schema_hash = org_rowset_meta->tablet_schema_hash();
    context.rowset_type = org_rowset_meta->rowset_type();
    context.rowset_path_prefix = new_path;
    context.tablet_schema = &tablet_schema;
    context.rowset_state = org_rowset_meta->rowset_state();
    context.version = org_rowset_meta->version();
    context.version_hash = org_rowset_meta->version_hash();
    // keep segments_overlap same as origin rowset
    context.segments_overlap = alpha_rowset_meta->segments_overlap();

    std::unique_ptr<RowsetWriter> rs_writer;
    RETURN_NOT_OK(RowsetFactory::create_rowset_writer(context, &rs_writer));

    res = rs_writer->add_rowset(org_rowset);
    if (res != OLAP_SUCCESS) {
        LOG(WARNING) << "failed to add rowset "
                     << " id = " << org_rowset->rowset_id() << " to rowset " << rowset_id;
        return res;
    }
    RowsetSharedPtr new_rowset = rs_writer->build();
    if (new_rowset == nullptr) {
        LOG(WARNING) << "failed to build rowset when rename rowset id";
        return OLAP_ERR_MALLOC_ERROR;
    }
    RETURN_NOT_OK(new_rowset->load());
    new_rowset->rowset_meta()->to_rowset_pb(new_rs_meta_pb);
    org_rowset->remove();
    return OLAP_SUCCESS;
}

// get snapshot path: curtime.seq.timeout
// eg: 20190819221234.3.86400
OLAPStatus SnapshotManager::_calc_snapshot_id_path(const TabletSharedPtr& tablet, int64_t timeout_s,
                                                   string* out_path) {
    OLAPStatus res = OLAP_SUCCESS;
    if (out_path == nullptr) {
        LOG(WARNING) << "output parameter cannot be NULL";
        return OLAP_ERR_INPUT_PARAMETER_ERROR;
    }

    // get current timestamp string
    string time_str;
    if ((res = gen_timestamp_string(&time_str)) != OLAP_SUCCESS) {
        LOG(WARNING) << "failed to generate time_string when move file to trash."
                     << "err code=" << res;
        return res;
    }

    std::stringstream snapshot_id_path_stream;
    MutexLock auto_lock(&_snapshot_mutex); // will automatically unlock when function return.
    snapshot_id_path_stream << tablet->data_dir()->path() << SNAPSHOT_PREFIX << "/" << time_str
                            << "." << _snapshot_base_id++ << "." << timeout_s;
    *out_path = snapshot_id_path_stream.str();
    return res;
}

// location: /path/to/data/DATA_PREFIX/shard_id
// return: /path/to/data/DATA_PREFIX/shard_id/tablet_id/schema_hash
std::string SnapshotManager::get_schema_hash_full_path(const TabletSharedPtr& ref_tablet,
                                                       const string& location) const {
    std::stringstream schema_full_path_stream;
    schema_full_path_stream << location << "/" << ref_tablet->tablet_id() << "/"
                            << ref_tablet->schema_hash();
    string schema_full_path = schema_full_path_stream.str();

    return schema_full_path;
}

std::string SnapshotManager::_get_header_full_path(const TabletSharedPtr& ref_tablet,
                                                   const std::string& schema_hash_path) const {
    std::stringstream header_name_stream;
    header_name_stream << schema_hash_path << "/" << ref_tablet->tablet_id() << ".hdr";
    return header_name_stream.str();
}

OLAPStatus SnapshotManager::_link_index_and_data_files(
        const string& schema_hash_path, const TabletSharedPtr& ref_tablet,
        const std::vector<RowsetSharedPtr>& consistent_rowsets) {
    OLAPStatus res = OLAP_SUCCESS;
    for (auto& rs : consistent_rowsets) {
        RETURN_NOT_OK(rs->link_files_to(schema_hash_path, rs->rowset_id()));
    }
    return res;
}

OLAPStatus SnapshotManager::_create_snapshot_files(const TabletSharedPtr& ref_tablet,
                                                   const TSnapshotRequest& request,
                                                   string* snapshot_path,
                                                   int32_t snapshot_version) {
    LOG(INFO) << "receive a make snapshot request,"
              << " request detail is " << apache::thrift::ThriftDebugString(request)
              << " snapshot_path is " << *snapshot_path << " snapshot_version is "
              << snapshot_version;
    OLAPStatus res = OLAP_SUCCESS;
    if (snapshot_path == nullptr) {
        LOG(WARNING) << "output parameter cannot be NULL";
        return OLAP_ERR_INPUT_PARAMETER_ERROR;
    }

    string snapshot_id_path;
    int64_t timeout_s = config::snapshot_expire_time_sec;
    if (request.__isset.timeout) {
        timeout_s = request.timeout;
    }
    res = _calc_snapshot_id_path(ref_tablet, timeout_s, &snapshot_id_path);
    if (res != OLAP_SUCCESS) {
        LOG(WARNING) << "failed to calc snapshot_id_path, ref tablet="
                     << ref_tablet->data_dir()->path();
        return res;
    }

    string schema_full_path = get_schema_hash_full_path(ref_tablet, snapshot_id_path);
    string header_path = _get_header_full_path(ref_tablet, schema_full_path);
    if (FileUtils::check_exist(schema_full_path)) {
        VLOG(10) << "remove the old schema_full_path.";
        FileUtils::remove_all(schema_full_path);
    }

    RETURN_WITH_WARN_IF_ERROR(FileUtils::create_dir(schema_full_path), OLAP_ERR_CANNOT_CREATE_DIR,
                              "create path " + schema_full_path + "failed");

    string snapshot_id;
    RETURN_WITH_WARN_IF_ERROR(FileUtils::canonicalize(snapshot_id_path, &snapshot_id),
                              OLAP_ERR_CANNOT_CREATE_DIR,
                              "canonicalize path " + snapshot_id_path + " failed");

    do {
        TabletMetaSharedPtr new_tablet_meta(new (nothrow) TabletMeta());
        if (new_tablet_meta == nullptr) {
            LOG(WARNING) << "fail to malloc TabletMeta.";
            res = OLAP_ERR_MALLOC_ERROR;
            break;
        }
        std::vector<RowsetSharedPtr> consistent_rowsets;
        if (request.__isset.missing_version) {
            ReadLock rdlock(ref_tablet->get_header_lock_ptr());
            for (int64_t missed_version : request.missing_version) {
                Version version = {missed_version, missed_version};
                const RowsetSharedPtr rowset = ref_tablet->get_inc_rowset_by_version(version);
                if (rowset != nullptr) {
                    consistent_rowsets.push_back(rowset);
                } else {
                    LOG(WARNING) << "failed to find missed version when snapshot. "
                                 << " tablet=" << request.tablet_id
                                 << " schema_hash=" << request.schema_hash
                                 << " version=" << version.first << "-" << version.second;
                    res = OLAP_ERR_VERSION_NOT_EXIST;
                    break;
                }
            }
            if (res != OLAP_SUCCESS) {
                break;
            }
            ref_tablet->generate_tablet_meta_copy_unlocked(new_tablet_meta);
        } else {
            ReadLock rdlock(ref_tablet->get_header_lock_ptr());
            // get latest version
            const RowsetSharedPtr last_version = ref_tablet->rowset_with_max_version();
            if (last_version == nullptr) {
                LOG(WARNING) << "tablet has not any version. path="
                             << ref_tablet->full_name().c_str();
                res = OLAP_ERR_VERSION_NOT_EXIST;
                break;
            }
            // get snapshot version, use request.version if specified
            int32_t version = last_version->end_version();
            if (request.__isset.version) {
                if (last_version->end_version() < request.version) {
                    LOG(WARNING) << "invalid make snapshot request. "
                                 << " version=" << last_version->end_version()
                                 << " req_version=" << request.version;
                    res = OLAP_ERR_INPUT_PARAMETER_ERROR;
                    break;
                }
                version = request.version;
            }
            // get shortest version path
            // it very important!!!!
            // it means 0-version has to be a readable version graph
            res = ref_tablet->capture_consistent_rowsets(Version(0, version), &consistent_rowsets);
            if (res != OLAP_SUCCESS) {
                LOG(WARNING) << "fail to select versions to span. res=" << res;
                break;
            }

            ref_tablet->generate_tablet_meta_copy_unlocked(new_tablet_meta);
        }

        std::vector<RowsetMetaSharedPtr> rs_metas;
        for (auto& rs : consistent_rowsets) {
            res = rs->link_files_to(schema_full_path, rs->rowset_id());
            if (res != OLAP_SUCCESS) {
                break;
            }
            rs_metas.push_back(rs->rowset_meta());
            VLOG(3) << "add rowset meta to clone list. "
                    << " start version " << rs->rowset_meta()->start_version() << " end version "
                    << rs->rowset_meta()->end_version() << " empty " << rs->rowset_meta()->empty();
        }
        if (res != OLAP_SUCCESS) {
            LOG(WARNING) << "fail to create hard link. [path=" << snapshot_id_path << "]";
            break;
        }

        // clear alter task info in snapshot files
        new_tablet_meta->delete_alter_task();

        if (request.__isset.missing_version) {
            new_tablet_meta->revise_inc_rs_metas(std::move(rs_metas));
            new_tablet_meta->revise_rs_metas(vector<RowsetMetaSharedPtr>());
        } else {
            // If this is a full clone, then should clear inc rowset metas because
            // related files is not created
            new_tablet_meta->revise_inc_rs_metas(vector<RowsetMetaSharedPtr>());
            new_tablet_meta->revise_rs_metas(std::move(rs_metas));
        }

        if (snapshot_version == g_Types_constants.TSNAPSHOT_REQ_VERSION1) {
            // convert beta rowset to alpha rowset
            if (request.__isset.missing_version) {
                res = _convert_beta_rowsets_to_alpha(new_tablet_meta,
                                                     new_tablet_meta->all_inc_rs_metas(),
                                                     schema_full_path, true);
            } else {
                res = _convert_beta_rowsets_to_alpha(
                        new_tablet_meta, new_tablet_meta->all_rs_metas(), schema_full_path, false);
            }
            if (res != OLAP_SUCCESS) {
                break;
            }
            res = new_tablet_meta->save(header_path);
            LOG(INFO) << "finish convert beta to alpha, res:" << res
                      << ", tablet:" << new_tablet_meta->tablet_id()
                      << ", schema hash:" << new_tablet_meta->schema_hash();
        } else if (snapshot_version == g_Types_constants.TSNAPSHOT_REQ_VERSION2) {
            res = new_tablet_meta->save(header_path);
        } else {
            res = OLAP_ERR_INVALID_SNAPSHOT_VERSION;
        }

        if (res != OLAP_SUCCESS) {
            LOG(WARNING) << "convert rowset failed, res:" << res
                         << ", tablet:" << new_tablet_meta->tablet_id()
                         << ", schema hash:" << new_tablet_meta->schema_hash()
                         << ", snapshot_version:" << snapshot_version
                         << ", is incremental:" << request.__isset.missing_version;
            break;
        }

        // append a single delta if request.version is end_version of cumulative delta
        if (request.__isset.version) {
            for (auto& rs : consistent_rowsets) {
                if (rs->end_version() == request.version) {
                    if (rs->start_version() != request.version) {
                        // visible version in fe is 900
                        // A need to clone 900 from B, but B's last version is 901, and 901 is not a visible version
                        // and 901 will be reverted
                        // since 900 is not the last version in B, 900 maybe compacted with other versions
                        // if A only get 900, then A's last version will be a cumulative delta
                        // many codes in be assumes that the last version is a single delta
                        // both clone and backup restore depend on this logic
                        // TODO (yiguolei) fix it in the future
                        // res = _append_single_delta(request, data_dir);
                        if (res != OLAP_SUCCESS) {
                            LOG(WARNING) << "fail to append single delta. res=" << res;
                        }
                    }
                    break;
                }
            }
        }
    } while (0);

    if (res != OLAP_SUCCESS) {
        LOG(WARNING) << "fail to make snapshot, try to delete the snapshot path. path="
                     << snapshot_id_path.c_str();

        if (FileUtils::check_exist(snapshot_id_path)) {
            VLOG(3) << "remove snapshot path. [path=" << snapshot_id_path << "]";
            FileUtils::remove_all(snapshot_id_path);
        }
    } else {
        *snapshot_path = snapshot_id;
    }

    return res;
}

OLAPStatus SnapshotManager::_convert_beta_rowsets_to_alpha(
        const TabletMetaSharedPtr& new_tablet_meta,
        const std::vector<RowsetMetaSharedPtr>& rowset_metas, const std::string& dst_path,
        bool is_incremental) {
    OLAPStatus res = OLAP_SUCCESS;
    RowsetConverter rowset_converter(new_tablet_meta);
    std::vector<RowsetMetaSharedPtr> new_rowset_metas;
    bool modified = false;
    for (auto& rowset_meta : rowset_metas) {
        if (rowset_meta->rowset_type() == BETA_ROWSET) {
            modified = true;
            RowsetMetaPB rowset_meta_pb;
            auto st =
                    rowset_converter.convert_beta_to_alpha(rowset_meta, dst_path, &rowset_meta_pb);
            if (st != OLAP_SUCCESS) {
                res = st;
                LOG(WARNING) << "convert beta to alpha failed"
                             << ", tablet_id:" << new_tablet_meta->tablet_id()
                             << ", schema hash:" << new_tablet_meta->schema_hash()
                             << ", src rowset:" << rowset_meta->rowset_id() << ", error:" << st;
                break;
            }
            RowsetMetaSharedPtr new_rowset_meta(new AlphaRowsetMeta());
            bool ret = new_rowset_meta->init_from_pb(rowset_meta_pb);
            if (!ret) {
                res = OLAP_ERR_INIT_FAILED;
                break;
            }
            new_rowset_metas.push_back(new_rowset_meta);
        } else {
            new_rowset_metas.push_back(rowset_meta);
        }
    }
    if (res == OLAP_SUCCESS && modified) {
        if (is_incremental) {
            new_tablet_meta->revise_inc_rs_metas(std::move(new_rowset_metas));
        } else {
            new_tablet_meta->revise_rs_metas(std::move(new_rowset_metas));
        }
    }
    return res;
}

} // namespace doris
