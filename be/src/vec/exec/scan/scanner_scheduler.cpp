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

#include "scanner_scheduler.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <ostream>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

#include "common/compiler_util.h" // IWYU pragma: keep
#include "common/config.h"
#include "common/logging.h"
#include "common/status.h"
#include "olap/tablet.h"
#include "runtime/exec_env.h"
#include "runtime/memory/mem_tracker.h"
#include "runtime/runtime_state.h"
#include "runtime/thread_context.h"
#include "util/async_io.h" // IWYU pragma: keep
#include "util/blocking_queue.hpp"
#include "util/cpu_info.h"
#include "util/defer_op.h"
#include "util/doris_metrics.h"
#include "util/metrics.h"
#include "util/runtime_profile.h"
#include "util/thread.h"
#include "util/threadpool.h"
#include "util/work_thread_pool.hpp"
#include "vec/core/block.h"
#include "vec/exec/scan/new_olap_scanner.h" // IWYU pragma: keep
#include "vec/exec/scan/scanner_context.h"
#include "vec/exec/scan/vscan_node.h"
#include "vec/exec/scan/vscanner.h"
#include "vfile_scanner.h"

namespace doris::vectorized {

ScannerScheduler::ScannerScheduler() = default;

ScannerScheduler::~ScannerScheduler() = default;

void ScannerScheduler::stop() {
    if (!_is_init) {
        return;
    }

    _is_closed = true;

    _limited_scan_thread_pool->shutdown();
    _limited_scan_thread_pool->wait();

    _local_scan_thread_pool->stop();
    _remote_scan_thread_pool->stop();

    LOG(INFO) << "ScannerScheduler stopped";
}

Status ScannerScheduler::init(ExecEnv* env) {
    // 1. local scan thread pool
    _local_scan_thread_pool =
            std::make_unique<vectorized::SimplifiedScanScheduler>("local_scan", nullptr);
    Status ret1 = _local_scan_thread_pool->start(config::doris_scanner_thread_pool_thread_num,
                                                 config::doris_scanner_thread_pool_thread_num,
                                                 config::doris_scanner_thread_pool_queue_size);
    RETURN_IF_ERROR(ret1);

    // 2. remote scan thread pool
    _remote_thread_pool_max_thread_num = ScannerScheduler::get_remote_scan_thread_num();
    int remote_scan_pool_queue_size = ScannerScheduler::get_remote_scan_thread_queue_size();
    _remote_scan_thread_pool =
            std::make_unique<vectorized::SimplifiedScanScheduler>("RemoteScanThreadPool", nullptr);
    Status ret2 = _remote_scan_thread_pool->start(_remote_thread_pool_max_thread_num,
                                                  config::doris_scanner_min_thread_pool_thread_num,
                                                  remote_scan_pool_queue_size);
    RETURN_IF_ERROR(ret2);

    // 3. limited scan thread pool
    RETURN_IF_ERROR(ThreadPoolBuilder("LimitedScanThreadPool")
                            .set_min_threads(config::doris_scanner_thread_pool_thread_num)
                            .set_max_threads(config::doris_scanner_thread_pool_thread_num)
                            .set_max_queue_size(config::doris_scanner_thread_pool_queue_size)
                            .build(&_limited_scan_thread_pool));
    _is_init = true;
    return Status::OK();
}

Status ScannerScheduler::submit(std::shared_ptr<ScannerContext> ctx,
                                std::shared_ptr<ScanTask> scan_task) {
    if (ctx->done()) {
        return Status::OK();
    }
    auto task_lock = ctx->task_exec_ctx();
    if (task_lock == nullptr) {
        LOG(INFO) << "could not lock task execution context, query " << ctx->debug_string()
                  << " maybe finished";
        return Status::OK();
    }

    if (ctx->thread_token != nullptr) {
        std::shared_ptr<ScannerDelegate> scanner_delegate = scan_task->scanner.lock();
        if (scanner_delegate == nullptr) {
            return Status::OK();
        }

        scanner_delegate->_scanner->start_wait_worker_timer();
        auto s = ctx->thread_token->submit_func([scanner_ref = scan_task, ctx]() {
            auto status = [&] {
                RETURN_IF_CATCH_EXCEPTION(_scanner_scan(ctx, scanner_ref));
                return Status::OK();
            }();

            if (!status.ok()) {
                scanner_ref->set_status(status);
                ctx->push_back_scan_task(scanner_ref);
            }
        });
        if (!s.ok()) {
            scan_task->set_status(s);
            return s;
        }
    } else {
        std::shared_ptr<ScannerDelegate> scanner_delegate = scan_task->scanner.lock();
        if (scanner_delegate == nullptr) {
            return Status::OK();
        }

        scanner_delegate->_scanner->start_wait_worker_timer();
        TabletStorageType type = scanner_delegate->_scanner->get_storage_type();
        auto sumbit_task = [&]() {
            SimplifiedScanScheduler* scan_sched = ctx->get_scan_scheduler();
            auto work_func = [scanner_ref = scan_task, ctx]() {
                auto status = [&] {
                    RETURN_IF_CATCH_EXCEPTION(_scanner_scan(ctx, scanner_ref));
                    return Status::OK();
                }();

                if (!status.ok()) {
                    scanner_ref->set_status(status);
                    ctx->push_back_scan_task(scanner_ref);
                }
            };
            SimplifiedScanTask simple_scan_task = {work_func, ctx};
            return scan_sched->submit_scan_task(simple_scan_task);
        };

        Status submit_status = sumbit_task();
        if (!submit_status.ok()) {
            // User will see TooManyTasks error. It looks like a more reasonable error.
            Status scan_task_status = Status::TooManyTasks(
                    "Failed to submit scanner to scanner pool reason:" +
                    std::string(submit_status.msg()) + "|type:" + std::to_string(type));
            scan_task->set_status(scan_task_status);
            return scan_task_status;
        }
    }

    return Status::OK();
}

std::unique_ptr<ThreadPoolToken> ScannerScheduler::new_limited_scan_pool_token(
        ThreadPool::ExecutionMode mode, int max_concurrency) {
    return _limited_scan_thread_pool->new_token(mode, max_concurrency);
}

void ScannerScheduler::_scanner_scan(std::shared_ptr<ScannerContext> ctx,
                                     std::shared_ptr<ScanTask> scan_task) {
    auto task_lock = ctx->task_exec_ctx();
    if (task_lock == nullptr) {
        return;
    }

    ctx->update_peak_running_scanner(1);
    Defer defer([&] { ctx->update_peak_running_scanner(-1); });

    std::shared_ptr<ScannerDelegate> scanner_delegate = scan_task->scanner.lock();
    if (scanner_delegate == nullptr) {
        return;
    }

    VScannerSPtr& scanner = scanner_delegate->_scanner;
    SCOPED_ATTACH_TASK(scanner->runtime_state());
    // for cpu hard limit, thread name should not be reset
    if (ctx->_should_reset_thread_name) {
        Thread::set_self_name("_scanner_scan");
    }

#ifndef __APPLE__
    // The configuration item is used to lower the priority of the scanner thread,
    // typically employed to ensure CPU scheduling for write operations.
    if (config::scan_thread_nice_value != 0 && scanner->get_name() != VFileScanner::NAME) {
        Thread::set_thread_nice_value();
    }
#endif
    MonotonicStopWatch max_run_time_watch;
    max_run_time_watch.start();
    scanner->update_wait_worker_timer();
    scanner->start_scan_cpu_timer();
    Status status = Status::OK();
    bool eos = false;
    ASSIGN_STATUS_IF_CATCH_EXCEPTION(
            RuntimeState* state = ctx->state(); DCHECK(nullptr != state);
            if (!scanner->is_init()) {
                status = scanner->init();
                if (!status.ok()) {
                    eos = true;
                }
            }

            if (!eos && !scanner->is_open()) {
                status = scanner->open(state);
                if (!status.ok()) {
                    eos = true;
                }
                scanner->set_opened();
            }

            Status rf_status = scanner->try_append_late_arrival_runtime_filter();
            if (!rf_status.ok()) {
                LOG(WARNING) << "Failed to append late arrival runtime filter: "
                             << rf_status.to_string();
            }

            size_t raw_bytes_threshold = config::doris_scanner_row_bytes;
            size_t raw_bytes_read = 0; bool first_read = true; int64_t limit = scanner->limit();
            while (!eos && raw_bytes_read < raw_bytes_threshold) {
                if (UNLIKELY(ctx->done())) {
                    eos = true;
                    break;
                }
                if (max_run_time_watch.elapsed_time() >
                    config::doris_scanner_max_run_time_ms * 1e6) {
                    break;
                }
                BlockUPtr free_block = ctx->get_free_block(first_read);
                if (free_block == nullptr) {
                    break;
                }
                // We got a new created block or a reused block.
                status = scanner->get_block_after_projects(state, free_block.get(), &eos);
                first_read = false;
                if (!status.ok()) {
                    LOG(WARNING) << "Scan thread read VScanner failed: " << status.to_string();
                    break;
                }
                // Projection will truncate useless columns, makes block size change.
                auto free_block_bytes = free_block->allocated_bytes();
                raw_bytes_read += free_block_bytes;
                if (!scan_task->cached_blocks.empty() &&
                    scan_task->cached_blocks.back().first->rows() + free_block->rows() <=
                            ctx->batch_size()) {
                    size_t block_size = scan_task->cached_blocks.back().first->allocated_bytes();
                    vectorized::MutableBlock mutable_block(
                            scan_task->cached_blocks.back().first.get());
                    status = mutable_block.merge(*free_block);
                    if (!status.ok()) {
                        LOG(WARNING) << "Block merge failed: " << status.to_string();
                        break;
                    }
                    scan_task->cached_blocks.back().second = mutable_block.allocated_bytes();
                    scan_task->cached_blocks.back().first.get()->set_columns(
                            std::move(mutable_block.mutable_columns()));

                    // Return block succeed or not, this free_block is not used by this scan task any more.
                    // If block can be reused, its memory usage will be added back.
                    ctx->return_free_block(std::move(free_block));
                    ctx->inc_block_usage(scan_task->cached_blocks.back().first->allocated_bytes() -
                                         block_size);
                } else {
                    ctx->inc_block_usage(free_block->allocated_bytes());
                    scan_task->cached_blocks.emplace_back(std::move(free_block), free_block_bytes);
                }
                if (limit > 0 && limit < ctx->batch_size()) {
                    // If this scanner has limit, and less than batch size,
                    // return immediately and no need to wait raw_bytes_threshold.
                    // This can save time that each scanner may only return a small number of rows,
                    // but rows are enough from all scanners.
                    // If not break, the query like "select * from tbl where id=1 limit 10"
                    // may scan a lot data when the "id=1"'s filter ratio is high.
                    // If limit is larger than batch size, this rule is skipped,
                    // to avoid user specify a large limit and causing too much small blocks.
                    break;
                }
            } // end for while

            if (UNLIKELY(!status.ok())) {
                scan_task->set_status(status);
                eos = true;
            },
            status);

    if (UNLIKELY(!status.ok())) {
        scan_task->set_status(status);
        eos = true;
    }

    scanner->update_scan_cpu_timer();
    if (eos) {
        scanner->mark_to_need_to_close();
    }
    scan_task->set_eos(eos);
    ctx->push_back_scan_task(scan_task);
}

int ScannerScheduler::get_remote_scan_thread_num() {
    int remote_max_thread_num = config::doris_max_remote_scanner_thread_pool_thread_num != -1
                                        ? config::doris_max_remote_scanner_thread_pool_thread_num
                                        : std::max(512, CpuInfo::num_cores() * 10);
    remote_max_thread_num =
            std::max(remote_max_thread_num, config::doris_scanner_thread_pool_thread_num);
    return remote_max_thread_num;
}

int ScannerScheduler::get_remote_scan_thread_queue_size() {
    return config::doris_remote_scanner_thread_pool_queue_size;
}

} // namespace doris::vectorized
