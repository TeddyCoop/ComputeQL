#define BUILD_ENTRY_DEFINING_UNIT 1
#define BUILD_CONSOLE_INTERFACE 1
#define PROFILE_CUSTOM 1
#define ARENA_FREE_LIST 1

#include "base/base_inc.h"
#include "os/os_inc.h"
#include <immintrin.h>

#include "base/base_inc.c"
#include "os/os_inc.c"

//~ tec: background load for the duckdb memory bandwidth test. run it next to bench_select_scale and stop it afterwards
//
//   --mode=read     every thread streams its own buffer, much bigger than the CPU caches, so the threads take system RAM bandwidth
//   --mode=spin     every thread runs an integer chain in registers, so the threads take CPU time and no memory bandwidth
//   --threads=N     default one per hardware thread
//   --mb_per_thread default 128
//   --seconds=S     default 3600, it prints its rate every 2 seconds until then

#define HOG_MAX_THREADS 64
#define HOG_REPORT_MILLISECONDS 2000
#define HOG_SPIN_STEPS 4096

typedef struct HOG_Worker HOG_Worker;
struct HOG_Worker
{
  U64* data;
  U64 word_count;
  B32 spin;
  volatile U64 work_done;
  volatile U64 stop;
  U64 sink;
};

internal void hog_read_pass(HOG_Worker* worker);
internal void hog_spin_steps(HOG_Worker* worker);
internal void hog_worker_run(void* ptr);
internal U64 hog_work_total(HOG_Worker* workers, U32 thread_count);

//~ tec: workers

// tec: one pass over the buffer, 128 bytes per step in four independent chains so the loads are limited by memory
internal void
hog_read_pass(HOG_Worker* worker)
{
  __m256i a0 = _mm256_setzero_si256();
  __m256i a1 = _mm256_setzero_si256();
  __m256i a2 = _mm256_setzero_si256();
  __m256i a3 = _mm256_setzero_si256();
  for (U64 i = 0; i + 16 <= worker->word_count; i += 16)
  {
    a0 = _mm256_add_epi64(a0, _mm256_loadu_si256((__m256i*)(worker->data + i + 0)));
    a1 = _mm256_add_epi64(a1, _mm256_loadu_si256((__m256i*)(worker->data + i + 4)));
    a2 = _mm256_add_epi64(a2, _mm256_loadu_si256((__m256i*)(worker->data + i + 8)));
    a3 = _mm256_add_epi64(a3, _mm256_loadu_si256((__m256i*)(worker->data + i + 12)));
  }
  __m256i pair0 = _mm256_add_epi64(a0, a1);
  __m256i pair1 = _mm256_add_epi64(a2, a3);
  __m256i sum = _mm256_add_epi64(pair0, pair1);
  U64 lanes[4];
  _mm256_storeu_si256((__m256i*)lanes, sum);
  worker->sink += lanes[0] + lanes[1] + lanes[2] + lanes[3];
}

internal void
hog_spin_steps(HOG_Worker* worker)
{
  U64 x = worker->sink | 1;
  for (U32 step = 0; step < HOG_SPIN_STEPS; step++)
  {
    x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    x = x ^ (x >> 29);
  }
  worker->sink = x;
}

// tec: work_done counts bytes read in read mode and spin steps in spin mode
internal void
hog_worker_run(void* ptr)
{
  HOG_Worker* worker = (HOG_Worker*)ptr;
  while (ins_atomic_u64_eval(&worker->stop) == 0)
  {
    if (worker->spin)
    {
      hog_spin_steps(worker);
      ins_atomic_u64_add_eval(&worker->work_done, HOG_SPIN_STEPS);
    }
    else
    {
      hog_read_pass(worker);
      ins_atomic_u64_add_eval(&worker->work_done, worker->word_count * sizeof(U64));
    }
  }
}

internal U64
hog_work_total(HOG_Worker* workers, U32 thread_count)
{
  U64 total = 0;
  for (U32 t = 0; t < thread_count; t++)
  {
    total += ins_atomic_u64_eval(&workers[t].work_done);
  }
  return total;
}

//~ tec: entry

internal void
entry_point(CmdLine* cmdline)
{
  ProfBeginCapture();

  U32 thread_count = os_get_system_info()->logical_processor_count;
  if (cmd_line_has_argument(cmdline, str8_lit("threads")))
  {
    thread_count = (U32)u64_from_str8(cmd_line_string(cmdline, str8_lit("threads")), 10);
  }
  thread_count = Clamp(1, thread_count, HOG_MAX_THREADS);
  U64 mb_per_thread = 128;
  if (cmd_line_has_argument(cmdline, str8_lit("mb_per_thread")))
  {
    mb_per_thread = u64_from_str8(cmd_line_string(cmdline, str8_lit("mb_per_thread")), 10);
  }
  U64 seconds = 3600;
  if (cmd_line_has_argument(cmdline, str8_lit("seconds")))
  {
    seconds = u64_from_str8(cmd_line_string(cmdline, str8_lit("seconds")), 10);
  }
  String8 mode = cmd_line_string(cmdline, str8_lit("mode"));
  B32 spin = str8_match(mode, str8_lit("spin"), 0);

  static HOG_Worker workers[HOG_MAX_THREADS];
  OS_Handle threads[HOG_MAX_THREADS];
  U64 word_count = MB(mb_per_thread) / sizeof(U64);
  for (U32 t = 0; t < thread_count; t++)
  {
    workers[t].spin = spin;
    workers[t].word_count = word_count;
    if (!spin)
    {
      U64 bytes = word_count * sizeof(U64);
      workers[t].data = (U64*)os_reserve(bytes);
      os_commit(workers[t].data, bytes);
      for (U64 i = 0; i < word_count; i++)
      {
        workers[t].data[i] = i * 2654435761ULL + t;
      }
    }
  }

  printf("ram hog: mode %s, %u threads, %llu MB per thread, %llu s\n", spin ? "spin" : "read", thread_count, spin ? 0ull : mb_per_thread, seconds);
  fflush(stdout);

  U64 start = os_now_microseconds();
  for (U32 t = 0; t < thread_count; t++)
  {
    threads[t] = os_thread_launch(hog_worker_run, &workers[t], 0);
  }

  U64 last_time = start;
  U64 last_work = 0;
  for (;;)
  {
    os_sleep_milliseconds(HOG_REPORT_MILLISECONDS);
    U64 now = os_now_microseconds();
    U64 work = hog_work_total(workers, thread_count);
    F64 interval_seconds = (F64)(now - last_time) / 1000000.0;
    F64 elapsed_seconds = (F64)(now - start) / 1000000.0;
    F64 rate = (F64)(work - last_work) / interval_seconds;
    if (spin)
    {
      printf("t=%7.1f s  %.2f G spin steps/s\n", elapsed_seconds, rate / 1e9);
    }
    else
    {
      printf("t=%7.1f s  %.2f GB/s read\n", elapsed_seconds, rate / 1e9);
    }
    fflush(stdout);
    last_time = now;
    last_work = work;
    if (elapsed_seconds >= (F64)seconds)
    {
      break;
    }
  }

  for (U32 t = 0; t < thread_count; t++)
  {
    ins_atomic_u64_add_eval(&workers[t].stop, 1);
  }
  U64 sink = 0;
  for (U32 t = 0; t < thread_count; t++)
  {
    os_thread_join(threads[t], max_U64);
    sink += workers[t].sink;
  }
  printf("ram hog done (%llu)\n", sink & 0xff);
}
