#include <benchmark/benchmark.h>
#include <platform.h>

#include <mem/heap.h>
#include <mem/pool.h>
#include <mem/pagemap.h>

class HeapBench: public ::benchmark::Fixture
{
  protected:
    heap_t _heap;

    virtual void SetUp(const ::benchmark::State& st);
    virtual void TearDown(const ::benchmark::State& st);
};

void HeapBench::SetUp(const ::benchmark::State& st)
{
  if (st.thread_index == 0) {
    ponyint_heap_init(&_heap);
  }
}

void HeapBench::TearDown(const ::benchmark::State& st)
{
  if (st.thread_index == 0) {
    ponyint_heap_destroy(&_heap);
  }
}

BENCHMARK_DEFINE_F(HeapBench, HeapAlloc$)(benchmark::State& st) {
  pony_actor_t* actor = (pony_actor_t*)0xDBEEFDEADBEEF;

  while (st.KeepRunning()) {
    if(st.range(0) > HEAP_MAX)
    {
      ponyint_heap_alloc_large(actor, &_heap, st.range(0));
    } else {
      ponyint_heap_alloc_small(actor, &_heap, ponyint_heap_index(st.range(0)));
    }
    st.PauseTiming();
    ponyint_heap_destroy(&_heap);
    ponyint_heap_init(&_heap);
    st.ResumeTiming();
  }
  st.SetItemsProcessed(st.iterations());
}

BENCHMARK_REGISTER_F(HeapBench, HeapAlloc$)->RangeMultiplier(2)->Ranges({{32, 1024<<10}});

BENCHMARK_DEFINE_F(HeapBench, HeapAlloc$_)(benchmark::State& st) {
  pony_actor_t* actor = (pony_actor_t*)0xDBEEFDEADBEEF;

  while (st.KeepRunning()) {
    if(st.range(0) > HEAP_MAX)
    {
      for(int i = 0; i < st.range(1); i++)
        ponyint_heap_alloc_large(actor, &_heap, st.range(0));
    } else {
      for(int i = 0; i < st.range(1); i++)
        ponyint_heap_alloc_small(actor, &_heap, ponyint_heap_index(st.range(0)));
    }
    st.PauseTiming();
    ponyint_heap_destroy(&_heap);
    ponyint_heap_init(&_heap);
    st.ResumeTiming();
  }
  st.SetItemsProcessed(st.iterations()*st.range(1));
}

BENCHMARK_REGISTER_F(HeapBench, HeapAlloc$_)->RangeMultiplier(2)->Ranges({{32, 1024<<10}, {1<<10, 1<<10}});

BENCHMARK_DEFINE_F(HeapBench, HeapDestroy$)(benchmark::State& st) {
  pony_actor_t* actor = (pony_actor_t*)0xDBEEFDEADBEEF;

  ponyint_heap_destroy(&_heap);
  while (st.KeepRunning()) {
    st.PauseTiming();
    ponyint_heap_init(&_heap);
    if(st.range(0) > HEAP_MAX)
      ponyint_heap_alloc_large(actor, &_heap, st.range(0));
    else
      ponyint_heap_alloc_small(actor, &_heap, ponyint_heap_index(st.range(0)));
    st.ResumeTiming();
    ponyint_heap_destroy(&_heap);
  }
  st.SetItemsProcessed(st.iterations());
  ponyint_heap_init(&_heap);
}

BENCHMARK_REGISTER_F(HeapBench, HeapDestroy$)->RangeMultiplier(2)->Ranges({{32, 1024<<10}});

BENCHMARK_DEFINE_F(HeapBench, HeapDestroyMultiple$)(benchmark::State& st) {
  pony_actor_t* actor = (pony_actor_t*)0xDBEEFDEADBEEF;

  ponyint_heap_destroy(&_heap);
  while (st.KeepRunning()) {
    st.PauseTiming();
    ponyint_heap_init(&_heap);
    if(st.range(0) > HEAP_MAX)
      for(int i = 0; i < st.range(1); i++)
        ponyint_heap_alloc_large(actor, &_heap, st.range(0));
    else
      for(int i = 0; i < st.range(1); i++)
        ponyint_heap_alloc_small(actor, &_heap, ponyint_heap_index(st.range(0)));
    st.ResumeTiming();
    ponyint_heap_destroy(&_heap);
  }
  st.SetItemsProcessed(st.iterations());
  ponyint_heap_init(&_heap);
}

BENCHMARK_REGISTER_F(HeapBench, HeapDestroyMultiple$)->RangeMultiplier(2)->Ranges({{32, 1024<<10}, {1<<10, 1<<10}});

BENCHMARK_DEFINE_F(HeapBench, HeapInitAllocDestroy)(benchmark::State& st) {
  pony_actor_t* actor = (pony_actor_t*)0xDBEEFDEADBEEF;

  ponyint_heap_destroy(&_heap);
  while (st.KeepRunning()) {
    ponyint_heap_init(&_heap);
    if(st.range(0) > HEAP_MAX)
      ponyint_heap_alloc_large(actor, &_heap, st.range(0));
    else
      ponyint_heap_alloc_small(actor, &_heap, ponyint_heap_index(st.range(0)));
    ponyint_heap_destroy(&_heap);
  }
  st.SetItemsProcessed(st.iterations());
  ponyint_heap_init(&_heap);
}

BENCHMARK_REGISTER_F(HeapBench, HeapInitAllocDestroy)->RangeMultiplier(2)->Ranges({{32, 1024<<10}});
