// SPDX-License-Identifier: GPL-2.0
#include <linux/cpumask.h>
#include <linux/export.h>
#include <linux/migrate.h>
#include <linux/mmu_context.h>
#include <linux/sched.h>
#include <linux/sched/task.h>

#include "sched.h"

#ifdef CONFIG_GRR_SCHED

#define GRR_TIME_SLICE		((100 * HZ) / 1000)

int grr_core_group_map[NR_CPUS];
EXPORT_SYMBOL_GPL(grr_core_group_map);

static struct task_struct *pick_next_task_grr(struct rq *rq, struct task_struct *prev);

void init_grr_hierarchy(void)
{
	int cpu, idx = 0;
	int nr_cpus = num_present_cpus();
	int split = max(1, nr_cpus / 2);

	for_each_possible_cpu(cpu) {
		if (idx < split)
			grr_core_group_map[cpu] = GRR_DEFAULT;
		else
			grr_core_group_map[cpu] = GRR_PERFORMANCE;
		idx++;
	}
}

#ifdef CONFIG_SMP
static int find_first_cpu_in_group(int group, const struct cpumask *mask)
{
	int cpu;

	for_each_online_cpu(cpu) {
		if (grr_core_group_map[cpu] != group)
			continue;
		if (mask && !cpumask_test_cpu(cpu, mask))
			continue;
		return cpu;
	}

	return -1;
}

static int find_idlest_cpu_in_group(int group, const struct cpumask *mask)
{
	int cpu, best_cpu = -1;
	unsigned int min_load = UINT_MAX;

	for_each_online_cpu(cpu) {
		if (grr_core_group_map[cpu] != group)
			continue;
		if (mask && !cpumask_test_cpu(cpu, mask))
			continue;

		if (cpu_rq(cpu)->grr.nr_running < min_load) {
			min_load = cpu_rq(cpu)->grr.nr_running;
			best_cpu = cpu;
		}
	}

	return best_cpu;
}

static int grr_select_cpu_for_group(int group, const struct cpumask *mask)
{
	int cpu = find_idlest_cpu_in_group(group, mask);

	if (cpu >= 0)
		return cpu;

	return find_first_cpu_in_group(group, mask);
}
#endif

static void enqueue_task_grr(struct rq *rq, struct task_struct *p, int flags)
{
	list_add_tail(&p->grr.run_list, &rq->grr.queue);
	rq->grr.nr_running++;
	add_nr_running(rq, 1);
}

static bool dequeue_task_grr(struct rq *rq, struct task_struct *p, int flags)
{
	list_del_init(&p->grr.run_list);
	rq->grr.nr_running--;
	sub_nr_running(rq, 1);
	return true;
}

static void yield_task_grr(struct rq *rq)
{
	struct task_struct *curr = rq->curr;

	list_move_tail(&curr->grr.run_list, &rq->grr.queue);
}

static void wakeup_preempt_grr(struct rq *rq, struct task_struct *p, int flags)
{
	/* Simple RR: no priority-based preemption */
}

static struct task_struct *pick_next_task_grr(struct rq *rq, struct task_struct *prev)
{
	struct sched_grr_entity *se;

	if (list_empty(&rq->grr.queue))
		return NULL;

	se = list_first_entry(&rq->grr.queue, struct sched_grr_entity, run_list);

	if (se->time_slice == 0)
		se->time_slice = GRR_TIME_SLICE;

	return container_of(se, struct task_struct, grr);
}

static void put_prev_task_grr(struct rq *rq, struct task_struct *prev, struct task_struct *next)
{
}

static void set_next_task_grr(struct rq *rq, struct task_struct *p, bool first)
{
	p->grr.time_slice = GRR_TIME_SLICE;
	p->se.exec_start = rq_clock_task(rq);
}

static void task_tick_grr(struct rq *rq, struct task_struct *curr, int queued)
{
	if (--curr->grr.time_slice > 0)
		return;

	curr->grr.time_slice = GRR_TIME_SLICE;
	if (rq->grr.nr_running > 1) {
		list_move_tail(&curr->grr.run_list, &rq->grr.queue);
		resched_curr(rq);
	}
}

static int select_task_rq_grr(struct task_struct *p, int cpu, int flags)
{
#ifdef CONFIG_SMP
	int group = p->grr.group ? p->grr.group : GRR_DEFAULT;
	int target = grr_select_cpu_for_group(group, p->cpus_ptr);

	if (target >= 0)
		return target;
#endif
	return cpu;
}

static void switched_to_grr(struct rq *rq, struct task_struct *p)
{
	if (p->grr.group == 0)
		p->grr.group = GRR_DEFAULT;
	resched_curr(rq);
}

static void prio_changed_grr(struct rq *rq, struct task_struct *p, int oldprio)
{
}

static void update_curr_grr(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	u64 now, delta_exec;

	if (curr->policy != SCHED_GRR)
		return;

	now = rq_clock_task(rq);
	delta_exec = now - curr->se.exec_start;
	if ((s64)delta_exec <= 0)
		return;

	curr->se.exec_start = now;
	curr->se.sum_exec_runtime += delta_exec;
}

void grr_load_balance(struct rq *this_rq)
{
#ifdef CONFIG_SMP
	int this_cpu = cpu_of(this_rq);
	int group = grr_core_group_map[this_cpu];
	int cpu, busiest = -1;
	unsigned int max_load = 0;
	struct rq *busiest_rq;
	struct sched_grr_entity *se;
	struct task_struct *p = NULL;

	for_each_online_cpu(cpu) {
		struct rq *rq;

		if (cpu == this_cpu)
			continue;
		if (grr_core_group_map[cpu] != group)
			continue;

		rq = cpu_rq(cpu);
		if (rq->grr.nr_running > max_load) {
			max_load = rq->grr.nr_running;
			busiest = cpu;
		}
	}

	if (busiest == -1)
		return;

	busiest_rq = cpu_rq(busiest);
	if (max_load <= this_rq->grr.nr_running + 1)
		return;

	double_rq_lock(this_rq, busiest_rq);

	if (busiest_rq->grr.nr_running <= 1) {
		double_rq_unlock(this_rq, busiest_rq);
		return;
	}

	list_for_each_entry(se, &busiest_rq->grr.queue, run_list) {
		p = container_of(se, struct task_struct, grr);
		if (p == busiest_rq->curr)
			continue;
		if (cpumask_test_cpu(this_cpu, p->cpus_ptr))
			break;
		p = NULL;
	}

	if (p) {
		deactivate_task(busiest_rq, p, 0);
		set_task_cpu(p, this_cpu);
		activate_task(this_rq, p, 0);
	}

	double_rq_unlock(this_rq, busiest_rq);
#endif
}

void grr_move_task_to_allowed_cpu(struct task_struct *p)
{
#ifdef CONFIG_SMP
	int dst;
	int group = p->grr.group ? p->grr.group : GRR_DEFAULT;

	if (!p || p->policy != SCHED_GRR)
		return;

	dst = grr_select_cpu_for_group(group, p->cpus_ptr);
	if (dst < 0 || dst == task_cpu(p))
		return;

	if (migrate_task_to(p, dst))
		pr_warn_once("sched_grr: failed to migrate task %s (%d) to cpu %d\n",
			     p->comm, p->pid, dst);
#endif
}

void grr_rebalance_cpu(int cpu)
{
#ifdef CONFIG_SMP
	for (;;) {
		struct sched_grr_entity *se;
		struct task_struct *p = NULL;
		struct rq *rq = cpu_rq(cpu);
		struct rq_flags rf;

		rq_lock_irqsave(rq, &rf);
		list_for_each_entry(se, &rq->grr.queue, run_list) {
			struct task_struct *candidate =
				container_of(se, struct task_struct, grr);

			if (candidate->grr.group == grr_core_group_map[cpu])
				continue;

			get_task_struct(candidate);
			p = candidate;
			break;
		}
		rq_unlock_irqrestore(rq, &rf);

		if (!p)
			break;

		grr_move_task_to_allowed_cpu(p);
		put_task_struct(p);
	}
#endif
}

static unsigned int get_rr_interval_grr(struct rq *rq, struct task_struct *task)
{
	return GRR_TIME_SLICE;
}

DEFINE_SCHED_CLASS(grr) = {
	.queue_mask		= 4,

	.enqueue_task		= enqueue_task_grr,
	.dequeue_task		= dequeue_task_grr,
	.yield_task		= yield_task_grr,

	.wakeup_preempt		= wakeup_preempt_grr,

	.pick_next_task		= pick_next_task_grr,
	.put_prev_task		= put_prev_task_grr,
	.set_next_task		= set_next_task_grr,

	.select_task_rq		= select_task_rq_grr,

	.task_tick		= task_tick_grr,
	.switched_to		= switched_to_grr,
	.prio_changed		= prio_changed_grr,
	.update_curr		= update_curr_grr,
	.get_rr_interval	= get_rr_interval_grr,
};

#endif /* CONFIG_GRR_SCHED */

