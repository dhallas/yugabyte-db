// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/yql/pggate/pg_client.h"

#include "yb/client/client-internal.h"
#include "yb/client/table.h"

#include "yb/rpc/poller.h"

#include "yb/tserver/pg_client.proxy.h"
#include "yb/tserver/tserver_shared_mem.h"

#include "yb/yql/pggate/pg_tabledesc.h"

DECLARE_bool(use_node_hostname_for_local_tserver);
DECLARE_int32(yb_client_admin_operation_timeout_sec);

DEFINE_uint64(pg_client_heartbeat_interval_ms, 10000, "Pg client heartbeat interval in ms.");

using namespace std::literals;

namespace yb {
namespace pggate {

class PgClient::Impl {
 public:
  Impl() : heartbeat_poller_(std::bind(&Impl::Heartbeat, this, false)) {}

  ~Impl() {
    CHECK(!proxy_);
  }

  CHECKED_STATUS Start(rpc::ProxyCache* proxy_cache,
                       rpc::Scheduler* scheduler,
                       const tserver::TServerSharedObject& tserver_shared_object) {
    CHECK_NOTNULL(&tserver_shared_object);
    MonoDelta resolve_cache_timeout;
    const auto& tserver_shared_data_ = *tserver_shared_object;
    HostPort host_port(tserver_shared_data_.endpoint());
    if (FLAGS_use_node_hostname_for_local_tserver) {
      host_port = HostPort(tserver_shared_data_.host().ToBuffer(),
                           tserver_shared_data_.endpoint().port());
      resolve_cache_timeout = MonoDelta::kMax;
    }
    LOG(INFO) << "Using TServer host_port: " << host_port;
    proxy_ = std::make_unique<tserver::PgClientServiceProxy>(
        proxy_cache, host_port, nullptr /* protocol */, resolve_cache_timeout);

    auto future = create_session_promise_.get_future();
    Heartbeat(true);
    RETURN_NOT_OK(future.get());
    heartbeat_poller_.Start(scheduler, FLAGS_pg_client_heartbeat_interval_ms * 1ms - 1s);
    return Status::OK();
  }

  void Shutdown() {
    heartbeat_poller_.Shutdown();
    proxy_ = nullptr;
  }

  void Heartbeat(bool create) {
    {
      bool expected = false;
      if (!heartbeat_running_.compare_exchange_strong(expected, true)) {
        LOG(DFATAL) << "Heartbeat did not complete yet";
        return;
      }
    }
    tserver::PgHeartbeatRequestPB req;
    req.set_create(create);
    req.set_session_id(session_id_);
    proxy_->HeartbeatAsync(
        req, &heartbeat_resp_, PrepareHeartbeatController(),
        [create, this] {
      auto status = ResponseStatus(heartbeat_resp_);
      heartbeat_running_ = false;
      if (create) {
        create_session_promise_.set_value(status);
      } else if (!status.ok()) {
        LOG(WARNING) << "Heartbeat failed: " << status;
      }
    });
  }

  CHECKED_STATUS AlterTable(tserver::PgAlterTableRequestPB* req, CoarseTimePoint deadline) {
    tserver::PgAlterTableResponsePB resp;
    req->set_session_id(session_id_);

    RETURN_NOT_OK(proxy_->AlterTable(*req, &resp, PrepareAdminController(deadline)));
    return ResponseStatus(resp);
  }


  CHECKED_STATUS CreateDatabase(tserver::PgCreateDatabaseRequestPB* req, CoarseTimePoint deadline) {
    tserver::PgCreateDatabaseResponsePB resp;
    req->set_session_id(session_id_);

    RETURN_NOT_OK(proxy_->CreateDatabase(*req, &resp, PrepareAdminController(deadline)));
    return ResponseStatus(resp);
  }

  CHECKED_STATUS CreateTable(tserver::PgCreateTableRequestPB* req, CoarseTimePoint deadline) {
    tserver::PgCreateTableResponsePB resp;
    req->set_session_id(session_id_);

    RETURN_NOT_OK(proxy_->CreateTable(*req, &resp, PrepareAdminController(deadline)));
    return ResponseStatus(resp);
  }

  Result<PgTableDescPtr> OpenTable(const PgObjectId& table_id) {
    tserver::PgOpenTableRequestPB req;
    req.set_table_id(table_id.GetYBTableId());
    tserver::PgOpenTableResponsePB resp;

    RETURN_NOT_OK(proxy_->OpenTable(req, &resp, PrepareAdminController()));
    RETURN_NOT_OK(ResponseStatus(resp));

    client::YBTableInfo info;
    RETURN_NOT_OK(client::CreateTableInfoFromTableSchemaResp(resp.info(), &info));

    auto partitions = std::make_shared<client::VersionedTablePartitionList>();
    partitions->version = resp.partitions().version();
    partitions->keys.assign(resp.partitions().keys().begin(), resp.partitions().keys().end());

    return make_scoped_refptr<PgTableDesc>(std::make_shared<client::YBTable>(
        info, std::move(partitions)));
  }

  Result<master::GetNamespaceInfoResponsePB> GetDatabaseInfo(uint32_t oid) {
    tserver::PgGetDatabaseInfoRequestPB req;
    req.set_oid(oid);

    tserver::PgGetDatabaseInfoResponsePB resp;

    RETURN_NOT_OK(proxy_->GetDatabaseInfo(req, &resp, PrepareAdminController()));
    RETURN_NOT_OK(ResponseStatus(resp));
    return resp.info();
  }

  Result<std::pair<PgOid, PgOid>> ReserveOids(PgOid database_oid, PgOid next_oid, uint32_t count) {
    tserver::PgReserveOidsRequestPB req;
    req.set_database_oid(database_oid);
    req.set_next_oid(next_oid);
    req.set_count(count);

    tserver::PgReserveOidsResponsePB resp;

    RETURN_NOT_OK(proxy_->ReserveOids(req, &resp, PrepareAdminController()));
    RETURN_NOT_OK(ResponseStatus(resp));
    return std::pair<PgOid, PgOid>(resp.begin_oid(), resp.end_oid());
  }

  Result<bool> IsInitDbDone() {
    tserver::PgIsInitDbDoneRequestPB req;
    tserver::PgIsInitDbDoneResponsePB resp;

    RETURN_NOT_OK(proxy_->IsInitDbDone(req, &resp, PrepareAdminController()));
    RETURN_NOT_OK(ResponseStatus(resp));
    return resp.done();
  }

 private:
  static rpc::RpcController* SetupAdminController(
      rpc::RpcController* controller, CoarseTimePoint deadline = CoarseTimePoint()) {
    if (deadline != CoarseTimePoint()) {
      controller->set_deadline(deadline);
    } else {
      controller->set_timeout(FLAGS_yb_client_admin_operation_timeout_sec * 1s);
    }
    return controller;
  }

  rpc::RpcController* PrepareAdminController(CoarseTimePoint deadline = CoarseTimePoint()) {
    controller_.Reset();
    return SetupAdminController(&controller_, deadline);
  }

  rpc::RpcController* PrepareHeartbeatController() {
    heartbeat_controller_.Reset();
    return SetupAdminController(
        &heartbeat_controller_,
        CoarseMonoClock::now() + FLAGS_pg_client_heartbeat_interval_ms * 1ms);
  }

  std::unique_ptr<tserver::PgClientServiceProxy> proxy_;
  rpc::RpcController controller_;
  const uint64_t session_id_ = RandomUniformInt<uint64_t>();

  rpc::Poller heartbeat_poller_;
  std::atomic<bool> heartbeat_running_{false};
  rpc::RpcController heartbeat_controller_;
  tserver::PgHeartbeatResponsePB heartbeat_resp_;
  std::promise<Status> create_session_promise_;
};

PgClient::PgClient() : impl_(new Impl) {
}

PgClient::~PgClient() {
}

Status PgClient::Start(
    rpc::ProxyCache* proxy_cache, rpc::Scheduler* scheduler,
    const tserver::TServerSharedObject& tserver_shared_object) {
  return impl_->Start(proxy_cache, scheduler, tserver_shared_object);
}

void PgClient::Shutdown() {
  impl_->Shutdown();
}

Status PgClient::AlterTable(tserver::PgAlterTableRequestPB* req, CoarseTimePoint deadline) {
  return impl_->AlterTable(req, deadline);
}

Status PgClient::CreateDatabase(tserver::PgCreateDatabaseRequestPB* req, CoarseTimePoint deadline) {
  return impl_->CreateDatabase(req, deadline);
}

Status PgClient::CreateTable(tserver::PgCreateTableRequestPB* req, CoarseTimePoint deadline) {
  return impl_->CreateTable(req, deadline);
}

Result<PgTableDescPtr> PgClient::OpenTable(const PgObjectId& table_id) {
  return impl_->OpenTable(table_id);
}

Result<master::GetNamespaceInfoResponsePB> PgClient::GetDatabaseInfo(uint32_t oid) {
  return impl_->GetDatabaseInfo(oid);
}

Result<std::pair<PgOid, PgOid>> PgClient::ReserveOids(
    PgOid database_oid, PgOid next_oid, uint32_t count) {
  return impl_->ReserveOids(database_oid, next_oid, count);
}

Result<bool> PgClient::IsInitDbDone() {
  return impl_->IsInitDbDone();
}

}  // namespace pggate
}  // namespace yb