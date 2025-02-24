// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/proxy_resolver/proxy_resolver_factory_impl.h"

#include <string>
#include <utility>

#include "base/bind.h"
#include "base/macros.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "net/base/net_errors.h"
#include "net/proxy_resolution/proxy_resolver_factory.h"
#include "net/proxy_resolution/proxy_resolver_v8_tracing.h"
#include "services/proxy_resolver/mojo_proxy_resolver_v8_tracing_bindings.h"
#include "services/proxy_resolver/proxy_resolver_impl.h"

namespace proxy_resolver {

class ProxyResolverFactoryImpl::Job {
 public:
  Job(ProxyResolverFactoryImpl* parent,
      const scoped_refptr<net::PacFileData>& pac_script,
      net::ProxyResolverV8TracingFactory* proxy_resolver_factory,
      mojo::PendingReceiver<mojom::ProxyResolver> receiver,
      mojo::PendingRemote<mojom::ProxyResolverFactoryRequestClient> client,
      std::unique_ptr<service_manager::ServiceKeepaliveRef>
          service_keepalive_ref);
  ~Job();

 private:
  void OnDisconnect();
  void OnProxyResolverCreated(int error);

  ProxyResolverFactoryImpl* const parent_;
  std::unique_ptr<net::ProxyResolverV8Tracing> proxy_resolver_impl_;
  mojo::PendingReceiver<mojom::ProxyResolver> proxy_receiver_;
  net::ProxyResolverV8TracingFactory* factory_;
  std::unique_ptr<net::ProxyResolverFactory::Request> request_;
  mojo::Remote<mojom::ProxyResolverFactoryRequestClient> remote_client_;
  std::unique_ptr<service_manager::ServiceKeepaliveRef> service_keepalive_ref_;

  DISALLOW_COPY_AND_ASSIGN(Job);
};

ProxyResolverFactoryImpl::Job::Job(
    ProxyResolverFactoryImpl* factory,
    const scoped_refptr<net::PacFileData>& pac_script,
    net::ProxyResolverV8TracingFactory* proxy_resolver_factory,
    mojo::PendingReceiver<mojom::ProxyResolver> receiver,
    mojo::PendingRemote<mojom::ProxyResolverFactoryRequestClient> client,
    std::unique_ptr<service_manager::ServiceKeepaliveRef> service_keepalive_ref)
    : parent_(factory),
      proxy_receiver_(std::move(receiver)),
      factory_(proxy_resolver_factory),
      remote_client_(std::move(client)),
      service_keepalive_ref_(std::move(service_keepalive_ref)) {
  remote_client_.set_disconnect_handler(base::BindOnce(
      &ProxyResolverFactoryImpl::Job::OnDisconnect, base::Unretained(this)));
  factory_->CreateProxyResolverV8Tracing(
      pac_script,
      std::make_unique<MojoProxyResolverV8TracingBindings<
          mojom::ProxyResolverFactoryRequestClient>>(remote_client_.get()),
      &proxy_resolver_impl_,
      base::BindOnce(&ProxyResolverFactoryImpl::Job::OnProxyResolverCreated,
                     base::Unretained(this)),
      &request_);
}

ProxyResolverFactoryImpl::Job::~Job() = default;

void ProxyResolverFactoryImpl::Job::OnDisconnect() {
  remote_client_->ReportResult(net::ERR_PAC_SCRIPT_TERMINATED);
  parent_->RemoveJob(this);
}

void ProxyResolverFactoryImpl::Job::OnProxyResolverCreated(int error) {
  if (error == net::OK) {
    parent_->AddResolver(
        std::make_unique<ProxyResolverImpl>(std::move(proxy_resolver_impl_),
                                            std::move(service_keepalive_ref_)),
        std::move(proxy_receiver_));
  }
  remote_client_->ReportResult(error);
  parent_->RemoveJob(this);
}

ProxyResolverFactoryImpl::ProxyResolverFactoryImpl()
    : ProxyResolverFactoryImpl(
          net::ProxyResolverV8TracingFactory::Create()) {}

void ProxyResolverFactoryImpl::BindReceiver(
    mojo::PendingReceiver<proxy_resolver::mojom::ProxyResolverFactory> receiver,
    service_manager::ServiceKeepalive* service_keepalive) {
  if (receivers_.empty()) {
    DCHECK(!service_keepalive_ref_);
    service_keepalive_ref_ = service_keepalive->CreateRef();
  }

  DCHECK(service_keepalive_ref_.get());
  receivers_.Add(this, std::move(receiver));
}

void ProxyResolverFactoryImpl::AddResolver(
    std::unique_ptr<mojom::ProxyResolver> resolver,
    mojo::PendingReceiver<mojom::ProxyResolver> receiver) {
  resolvers_.Add(std::move(resolver), std::move(receiver));
}

ProxyResolverFactoryImpl::ProxyResolverFactoryImpl(
    std::unique_ptr<net::ProxyResolverV8TracingFactory> proxy_resolver_factory)
    : proxy_resolver_impl_factory_(std::move(proxy_resolver_factory)) {
  receivers_.set_disconnect_handler(base::BindRepeating(
      &ProxyResolverFactoryImpl::OnDisconnect, base::Unretained(this)));
}

ProxyResolverFactoryImpl::~ProxyResolverFactoryImpl() = default;

void ProxyResolverFactoryImpl::CreateResolver(
    const std::string& pac_script,
    mojo::PendingReceiver<mojom::ProxyResolver> receiver,
    mojo::PendingRemote<mojom::ProxyResolverFactoryRequestClient> client) {
  DCHECK(service_keepalive_ref_);

  // The Job will call RemoveJob on |this| when either the create request
  // finishes or |receiver| or |client| encounters a connection error.
  std::unique_ptr<Job> job = std::make_unique<Job>(
      this, net::PacFileData::FromUTF8(pac_script),
      proxy_resolver_impl_factory_.get(), std::move(receiver),
      std::move(client), service_keepalive_ref_->Clone());
  Job* job_ptr = job.get();
  jobs_[job_ptr] = std::move(job);
}

void ProxyResolverFactoryImpl::RemoveJob(Job* job) {
  size_t erased_count = jobs_.erase(job);
  DCHECK_EQ(1U, erased_count);
}

void ProxyResolverFactoryImpl::OnDisconnect() {
  DCHECK(service_keepalive_ref_);
  if (receivers_.empty())
    service_keepalive_ref_.reset();
}

}  // namespace proxy_resolver
