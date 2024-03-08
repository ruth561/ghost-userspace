#pragma once

#include "schedulers/edf/edf_scheduler.h"
#include "shared/prio_table.h"
#include <memory>


namespace ghost {

// RUNNABLEフラグがセットされていればtrueを返す。
inline bool SchedItemRunnable(PrioTable* table, int sidx) {
  const struct sched_item* src = table->sched_item(sidx);
  uint32_t begin, flags;
  bool success = false;

  while (!success) {
    begin = src->seqcount.read_begin();
    flags = src->flags;
    success = src->seqcount.read_end(begin);
  }

  return flags & SCHED_ITEM_RUNNABLE;
}

// RUNNABLEフラグをクリアする。
inline void MarkSchedItemIdle(PrioTable* table, int sidx) {
  struct sched_item* si = table->sched_item(sidx);

  const uint32_t seq = si->seqcount.write_begin();
  si->flags &= ~SCHED_ITEM_RUNNABLE;
  si->seqcount.write_end(seq);
  table->MarkUpdatedIndex(sidx, /* num_retries = */ 3);
}

// RUNNABLEフラグをセットする。
inline void MarkSchedItemRunnable(PrioTable* table, int sidx) {
  struct sched_item* si = table->sched_item(sidx);

  const uint32_t seq = si->seqcount.write_begin();
  si->flags |= SCHED_ITEM_RUNNABLE;
  si->seqcount.write_end(seq);
  table->MarkUpdatedIndex(sidx, /* num_retries = */ 3);
}

// table->sched_item(sidx)の設定値を更新する関数。
inline void UpdateSchedItem(PrioTable* table, uint32_t sidx, uint32_t wcid,
                     uint32_t flags, const Gtid& gtid, absl::Duration d) {
  struct sched_item* si;

  si = table->sched_item(sidx);

  const uint32_t seq = si->seqcount.write_begin();
  si->sid = sidx;
  si->wcid = wcid;
  si->flags = flags;
  si->gpid = gtid.id();
  si->deadline = absl::ToUnixNanos(MonotonicNow() + d);
  si->seqcount.write_end(seq);
  table->MarkUpdatedIndex(sidx, /* num_retries = */ 3);
}

} // namespace ghost