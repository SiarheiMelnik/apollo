/******************************************************************************
 * Copyright 2018 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Vesched_infoon 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/
#include "cyber/scheduler/scheduler.h"

#include <utility>

#include "cyber/common/global_data.h"
#include "cyber/common/util.h"
#include "cyber/data/data_visitor.h"
#include "cyber/scheduler/policy/classic.h"
#include "cyber/scheduler/policy/task_choreo.h"
#include "cyber/scheduler/processor.h"
#include "cyber/scheduler/processor_context.h"

namespace apollo {
namespace cyber {
namespace scheduler {

using apollo::cyber::common::GlobalData;

Scheduler::Scheduler() : stop_(false) {
  auto gconf = GlobalData::Instance()->Config();
  proc_num_ = std::thread::hardware_concurrency();

  if (gconf.has_scheduler_conf()) {
    sched_conf_.CopyFrom(gconf.scheduler_conf());

    if (sched_conf_.has_processor_conf()) {
      sched_policy_ = sched_conf_.processor_conf().process_strategy();
      proc_num_ = sched_conf_.processor_conf().processor_num();
    } else {
      AERROR << "No processor conf";
      return;
    }

    if (sched_conf_.has_task_pool_conf()) {
      task_pool_size_ = sched_conf_.task_pool_conf().task_pool_size();
    } else {
      AERROR << "No processor conf";
      return;
    }

  } else {
    AERROR << "No scheduler conf";
    return;
  }

  for (auto& conf : gconf.routine_conf()) {
    if (rt_confs_.find(conf.routine_name()) ==
        rt_confs_.end()) {
      rt_confs_[conf.routine_name()] = conf;
    }
  }

  CreateProcessor();
  //  StartSysmon();
}

void Scheduler::StartSysmon() {
  int interval = 1000;
  struct sched_param param;
  int policy;

  if (sched_conf_.has_sysmon_hz()) {
    interval = (1.0 / sched_conf_.sysmon_hz()) * 1000000;
  }
  sysmon_ = std::thread([=]() {
    while (!stop_) {
      usleep(interval);
    }
  });

  pthread_getschedparam(sysmon_.native_handle(), &policy, &param);
  param.sched_priority = 60;
  if (sched_conf_.has_sysmon_prio()) {
    param.sched_priority = sched_conf_.sysmon_prio();
  }
  pthread_setschedparam(sysmon_.native_handle(), SCHED_FIFO, &param);
}

void Scheduler::CreateProcessor() {
  for (uint32_t i = 0; i < proc_num_ + task_pool_size_; i++) {
    auto proc = std::make_shared<Processor>();
    proc->set_id(i);

    std::shared_ptr<ProcessorContext> ctx;
    switch (sched_policy_) {
      case ProcessStrategy::CLASSIC:
        ctx.reset(new ClassicContext());
        break;
      case ProcessStrategy::CHOREO:
        ctx.reset(new TaskChoreoContext());
        break;
      default:
        ctx.reset(new TaskChoreoContext());
        break;
    }

    ctx->set_id(i);
    proc->bind_context(ctx);
    proc->set_strategy(sched_policy_);
    ctx->bind_processor(proc);
    proc_ctxs_.emplace_back(ctx);
    proc->Start();
  }
}

void Scheduler::ShutDown() {
  if (stop_.exchange(true)) {
    return;
  }

  for (auto& proc_ctx : proc_ctxs_) {
    proc_ctx->ShutDown();
  }
  proc_ctxs_.clear();

  if (sysmon_.joinable()) {
    sysmon_.join();
  }
}

Scheduler::~Scheduler() {}

std::shared_ptr<ProcessorContext> Scheduler::FindProc(
    const std::shared_ptr<CRoutine>& cr) {
  int cur_proc_ = -1;
  auto cur_cr = CRoutine::GetCurrentRoutine();

  if (cur_cr) {
    cur_proc_ = cur_cr->processor_id();
  }

  if (cr->processor_id() != -1 && cr->processor_id() != cur_proc_ &&
      cr->processor_id() < static_cast<int>(proc_ctxs_.size())) {
    cr->set_processor_id(proc_ctxs_[cr->processor_id()]->id());
    return proc_ctxs_[cr->processor_id()];
  }

  auto id = 0;
  int min_q_size = proc_ctxs_[id]->RqSize();
  for (uint32_t i = 1; i < proc_num_; i++) {
    if (min_q_size > proc_ctxs_[i]->RqSize()) {
      min_q_size = proc_ctxs_[i]->RqSize();
      id = i;
    }
  }
  cr->set_processor_id(proc_ctxs_[id]->id());
  return proc_ctxs_[cr->processor_id()];
}

bool Scheduler::DispatchTask(const std::shared_ptr<CRoutine>& croutine) {
  auto ctx = FindProc(croutine);

  if (!ctx) {
    AERROR << GlobalData::GetTaskNameById(croutine->id())
           << " push failed, get processor failed, "
           << "target processor index: " << croutine->processor_id();
    return false;
  }

  // rt may removed before inserted to croutine
  if (rt_ctx_.find(croutine->id()) != rt_ctx_.end()) {
    rt_ctx_[croutine->id()] = croutine->processor_id();
  }

  if (!ctx->Enqueue(croutine)) {
    AWARN << "push routine[" << GlobalData::GetTaskNameById(croutine->id())
          << "] into processor[" << croutine->processor_id() << "] failed";
    return false;
  }

  return true;
}

bool Scheduler::CreateTask(const RoutineFactory& factory,
                           const std::string& name) {
  return CreateTask(factory.create_routine(), name, factory.GetDataVisitor());
}

bool Scheduler::CreateTask(std::function<void()>&& func,
                           const std::string& name,
                           std::shared_ptr<DataVisitorBase> visitor) {
  if (stop_) {
    AERROR << "scheduler is stoped, cannot create task!";
    return false;
  }

  auto task_id = GlobalData::RegisterTaskName(name);
  {
    std::lock_guard<std::mutex> lg(rt_ctx_mutex_);
    if (rt_ctx_.find(task_id) != rt_ctx_.end()) {
      AERROR << "Routine [" << name << "] has been exists";
      return false;
    }
    rt_ctx_[task_id] = 0;
  }

  if (visitor != nullptr) {
    visitor->RegisterNotifyCallback([this, task_id, name]() {
      if (stop_) {
        return;
      }
      this->NotifyProcessor(task_id);
    });
  }

  RoutineConf rt_conf;
  if (rt_confs_.find(name) != rt_confs_.end()) {
    rt_conf = rt_confs_[name];
  }

  auto croutine = std::make_shared<CRoutine>(func);
  croutine->set_id(task_id);
  croutine->set_name(name);
  croutine->set_priority(rt_conf.priority());
  croutine->set_frequency(rt_conf.frequency());

  if (rt_conf.has_processor_index()) {
    auto processor_id = rt_conf.processor_index();
    croutine->set_processor_id(processor_id);
  }

  std::lock_guard<std::mutex> lg(rt_ctx_mutex_);
  if (!DispatchTask(croutine)) {
    return false;
  }

  return true;
}

bool Scheduler::NotifyTask(uint64_t task_id) {
  if (stop_) {
    return true;
  }

  return NotifyProcessor(task_id);
}

bool Scheduler::NotifyProcessor(uint64_t cr_id) {
  if (stop_) {
    return true;
  }

  std::lock_guard<std::mutex> lg(rt_ctx_mutex_);
  auto itr = rt_ctx_.find(cr_id);
  if (itr != rt_ctx_.end()) {
    proc_ctxs_[itr->second]->Notify(cr_id);
    return true;
  }

  return false;
}

bool Scheduler::RemoveTask(const std::string& name) {
  if (stop_) {
    return true;
  }

  auto task_id = GlobalData::RegisterTaskName(name);
  {
    std::lock_guard<std::mutex> lg(rt_ctx_mutex_);
    rt_ctx_.erase(task_id);
  }

  return RemoveCRoutine(task_id);
}

bool Scheduler::RemoveCRoutine(uint64_t cr_id) {
  if (stop_) {
    return true;
  }

  std::lock_guard<std::mutex> lg(rt_ctx_mutex_);
  auto p = rt_ctx_.find(cr_id);
  if (p != rt_ctx_.end()) {
    rt_ctx_.erase(cr_id);
    proc_ctxs_[p->second]->RemoveCRoutine(cr_id);
  }

  return true;
}

}  // namespace scheduler
}  // namespace cyber
}  // namespace apollo