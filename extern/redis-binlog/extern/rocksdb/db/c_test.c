/* Copyright (c) 2011 The LevelDB Authors. All rights reserved.
   Use of this source code is governed by a BSD-style license that can be
   found in the LICENSE file. See the AUTHORS file for names of contributors. */

#include "rocksdb/c.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

const char* phase = "";
static char dbname[200];

static void StartPhase(const char* name) {
  fprintf(stderr, "=== Test %s\n", name);
  phase = name;
}

static const char* GetTempDir(void) {
    const char* ret = getenv("TEST_TMPDIR");
    if (ret == NULL || ret[0] == '\0')
        ret = "/tmp";
    return ret;
}

#define CheckNoError(err)                                               \
  if ((err) != NULL) {                                                  \
    fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, phase, (err)); \
    abort();                                                            \
  }

#define CheckCondition(cond)                                            \
  if (!(cond)) {                                                        \
    fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, phase, #cond); \
    abort();                                                            \
  }

static void CheckEqual(const char* expected, const char* v, size_t n) {
  if (expected == NULL && v == NULL) {
    // ok
  } else if (expected != NULL && v != NULL && n == strlen(expected) &&
             memcmp(expected, v, n) == 0) {
    // ok
    return;
  } else {
    fprintf(stderr, "%s: expected '%s', got '%s'\n",
            phase,
            (expected ? expected : "(null)"),
            (v ? v : "(null"));
    abort();
  }
}

static void Free(char** ptr) {
  if (*ptr) {
    free(*ptr);
    *ptr = NULL;
  }
}

static void CheckGet(
    rocksdb_t* db,
    const rocksdb_readoptions_t* options,
    const char* key,
    const char* expected) {
  char* err = NULL;
  size_t val_len;
  char* val;
  val = rocksdb_get(db, options, key, strlen(key), &val_len, &err);
  CheckNoError(err);
  CheckEqual(expected, val, val_len);
  Free(&val);
}

static void CheckGetCF(
    rocksdb_t* db,
    const rocksdb_readoptions_t* options,
    rocksdb_column_family_handle_t* handle,
    const char* key,
    const char* expected) {
  char* err = NULL;
  size_t val_len;
  char* val;
  val = rocksdb_get_cf(db, options, handle, key, strlen(key), &val_len, &err);
  CheckNoError(err);
  CheckEqual(expected, val, val_len);
  Free(&val);
}


static void CheckIter(rocksdb_iterator_t* iter,
                      const char* key, const char* val) {
  size_t len;
  const char* str;
  str = rocksdb_iter_key(iter, &len);
  CheckEqual(key, str, len);
  str = rocksdb_iter_value(iter, &len);
  CheckEqual(val, str, len);
}

// Callback from rocksdb_writebatch_iterate()
static void CheckPut(void* ptr,
                     const char* k, size_t klen,
                     const char* v, size_t vlen) {
  int* state = (int*) ptr;
  CheckCondition(*state < 2);
  switch (*state) {
    case 0:
      CheckEqual("bar", k, klen);
      CheckEqual("b", v, vlen);
      break;
    case 1:
      CheckEqual("box", k, klen);
      CheckEqual("c", v, vlen);
      break;
  }
  (*state)++;
}

// Callback from rocksdb_writebatch_iterate()
static void CheckDel(void* ptr, const char* k, size_t klen) {
  int* state = (int*) ptr;
  CheckCondition(*state == 2);
  CheckEqual("bar", k, klen);
  (*state)++;
}

static void CmpDestroy(void* arg) { }

static int CmpCompare(void* arg, const char* a, size_t alen,
                      const char* b, size_t blen) {
  size_t n = (alen < blen) ? alen : blen;
  int r = memcmp(a, b, n);
  if (r == 0) {
    if (alen < blen) r = -1;
    else if (alen > blen) r = +1;
  }
  return r;
}

static const char* CmpName(void* arg) {
  return "foo";
}

// Custom filter policy
static unsigned char fake_filter_result = 1;
static void FilterDestroy(void* arg) { }
static const char* FilterName(void* arg) {
  return "TestFilter";
}
static char* FilterCreate(
    void* arg,
    const char* const* key_array, const size_t* key_length_array,
    int num_keys,
    size_t* filter_length) {
  *filter_length = 4;
  char* result = malloc(4);
  memcpy(result, "fake", 4);
  return result;
}
static unsigned char FilterKeyMatch(
    void* arg,
    const char* key, size_t length,
    const char* filter, size_t filter_length) {
  CheckCondition(filter_length == 4);
  CheckCondition(memcmp(filter, "fake", 4) == 0);
  return fake_filter_result;
}

// Custom compaction filter
static void CFilterDestroy(void* arg) {}
static const char* CFilterName(void* arg) { return "foo"; }
static unsigned char CFilterFilter(void* arg, int level, const char* key,
                                   size_t key_length,
                                   const char* existing_value,
                                   size_t value_length, char** new_value,
                                   size_t* new_value_length,
                                   unsigned char* value_changed) {
  if (key_length == 3) {
    if (memcmp(key, "bar", key_length) == 0) {
      return 1;
    } else if (memcmp(key, "baz", key_length) == 0) {
      *value_changed = 1;
      *new_value = "newbazvalue";
      *new_value_length = 11;
      return 0;
    }
  }
  return 0;
}

static void CFilterFactoryDestroy(void* arg) {}
static const char* CFilterFactoryName(void* arg) { return "foo"; }
static rocksdb_compactionfilter_t* CFilterCreate(
    void* arg, rocksdb_compactionfiltercontext_t* context) {
  return rocksdb_compactionfilter_create(NULL, CFilterDestroy, CFilterFilter,
                                         CFilterName);
}

static rocksdb_t* CheckCompaction(rocksdb_t* db, rocksdb_options_t* options,
                                  rocksdb_readoptions_t* roptions,
                                  rocksdb_writeoptions_t* woptions) {
  char* err = NULL;
  db = rocksdb_open(options, dbname, &err);
  CheckNoError(err);
  rocksdb_put(db, woptions, "foo", 3, "foovalue", 8, &err);
  CheckNoError(err);
  CheckGet(db, roptions, "foo", "foovalue");
  rocksdb_put(db, woptions, "bar", 3, "barvalue", 8, &err);
  CheckNoError(err);
  CheckGet(db, roptions, "bar", "barvalue");
  rocksdb_put(db, woptions, "baz", 3, "bazvalue", 8, &err);
  CheckNoError(err);
  CheckGet(db, roptions, "baz", "bazvalue");

  // Force compaction
  rocksdb_compact_range(db, NULL, 0, NULL, 0);
  // should have filtered bar, but not foo
  CheckGet(db, roptions, "foo", "foovalue");
  CheckGet(db, roptions, "bar", NULL);
  CheckGet(db, roptions, "baz", "newbazvalue");
  return db;
}

// Custom compaction filter V2.
static void CompactionFilterV2Destroy(void* arg) { }
static const char* CompactionFilterV2Name(void* arg) {
  return "TestCompactionFilterV2";
}
static void CompactionFilterV2Filter(
    void* arg, int level, size_t num_keys,
    const char* const* keys_list, const size_t* keys_list_sizes,
    const char* const* existing_values_list, const size_t* existing_values_list_sizes,
    char** new_values_list, size_t* new_values_list_sizes,
    unsigned char* to_delete_list) {
  size_t i;
  for (i = 0; i < num_keys; i++) {
    // If any value is "gc", it's removed.
    if (existing_values_list_sizes[i] == 2 && memcmp(existing_values_list[i], "gc", 2) == 0) {
      to_delete_list[i] = 1;
    } else if (existing_values_list_sizes[i] == 6 && memcmp(existing_values_list[i], "gc all", 6) == 0) {
      // If any value is "gc all", all keys are removed.
      size_t j;
      for (j = 0; j < num_keys; j++) {
        to_delete_list[j] = 1;
      }
      return;
    } else if (existing_values_list_sizes[i] == 6 && memcmp(existing_values_list[i], "change", 6) == 0) {
      // If value is "change", set changed value to "changed".
      size_t len;
      len = strlen("changed");
      new_values_list[i] = malloc(len);
      memcpy(new_values_list[i], "changed", len);
      new_values_list_sizes[i] = len;
    } else {
      // Otherwise, no keys are removed.
    }
  }
}

// Custom prefix extractor for compaction filter V2 which extracts first 3 characters.
static void CFV2PrefixExtractorDestroy(void* arg) { }
static char* CFV2PrefixExtractorTransform(void* arg, const char* key, size_t length, size_t* dst_length) {
  // Verify keys are maximum length 4; this verifies fix for a
  // prior bug which was passing the RocksDB-encoded key with
  // logical timestamp suffix instead of parsed user key.
  if (length > 4) {
    fprintf(stderr, "%s:%d: %s: key %s is not user key\n", __FILE__, __LINE__, phase, key);
    abort();
  }
  *dst_length = length < 3 ? length : 3;
  return (char*)key;
}
static unsigned char CFV2PrefixExtractorInDomain(void* state, const char* key, size_t length) {
  return 1;
}
static unsigned char CFV2PrefixExtractorInRange(void* state, const char* key, size_t length) {
  return 1;
}
static const char* CFV2PrefixExtractorName(void* state) {
  return "TestCFV2PrefixExtractor";
}

// Custom compaction filter factory V2.
static void CompactionFilterFactoryV2Destroy(void* arg) {
  rocksdb_slicetransform_destroy((rocksdb_slicetransform_t*)arg);
}
static const char* CompactionFilterFactoryV2Name(void* arg) {
  return "TestCompactionFilterFactoryV2";
}
static rocksdb_compactionfilterv2_t* CompactionFilterFactoryV2Create(
    void* state, const rocksdb_compactionfiltercontext_t* context) {
  return rocksdb_compactionfilterv2_create(state, CompactionFilterV2Destroy,
                                           CompactionFilterV2Filter,
                                           CompactionFilterV2Name);
}

// Custom merge operator
static void MergeOperatorDestroy(void* arg) { }
static const char* MergeOperatorName(void* arg) {
  return "TestMergeOperator";
}
static char* MergeOperatorFullMerge(
    void* arg,
    const char* key, size_t key_length,
    const char* existing_value, size_t existing_value_length,
    const char* const* operands_list, const size_t* operands_list_length,
    int num_operands,
    unsigned char* success, size_t* new_value_length) {
  *new_value_length = 4;
  *success = 1;
  char* result = malloc(4);
  memcpy(result, "fake", 4);
  return result;
}
static char* MergeOperatorPartialMerge(
    void* arg,
    const char* key, size_t key_length,
    const char* const* operands_list, const size_t* operands_list_length,
    int num_operands,
    unsigned char* success, size_t* new_value_length) {
  *new_value_length = 4;
  *success = 1;
  char* result = malloc(4);
  memcpy(result, "fake", 4);
  return result;
}

int main(int argc, char** argv) {
  rocksdb_t* db;
  rocksdb_comparator_t* cmp;
  rocksdb_cache_t* cache;
  rocksdb_env_t* env;
  rocksdb_options_t* options;
  rocksdb_block_based_table_options_t* table_options;
  rocksdb_readoptions_t* roptions;
  rocksdb_writeoptions_t* woptions;
  char* err = NULL;
  int run = -1;

  snprintf(dbname, sizeof(dbname),
           "%s/rocksdb_c_test-%d",
           GetTempDir(),
           ((int) geteuid()));

  StartPhase("create_objects");
  cmp = rocksdb_comparator_create(NULL, CmpDestroy, CmpCompare, CmpName);
  env = rocksdb_create_default_env();
  cache = rocksdb_cache_create_lru(100000);

  options = rocksdb_options_create();
  rocksdb_options_set_comparator(options, cmp);
  rocksdb_options_set_error_if_exists(options, 1);
  rocksdb_options_set_env(options, env);
  rocksdb_options_set_info_log(options, NULL);
  rocksdb_options_set_write_buffer_size(options, 100000);
  rocksdb_options_set_paranoid_checks(options, 1);
  rocksdb_options_set_max_open_files(options, 10);
  table_options = rocksdb_block_based_options_create();
  rocksdb_block_based_options_set_block_cache(table_options, cache);
  rocksdb_options_set_block_based_table_factory(options, table_options);

  rocksdb_options_set_compression(options, rocksdb_no_compression);
  rocksdb_options_set_compression_options(options, -14, -1, 0);
  int compression_levels[] = {rocksdb_no_compression, rocksdb_no_compression,
                              rocksdb_no_compression, rocksdb_no_compression};
  rocksdb_options_set_compression_per_level(options, compression_levels, 4);

  roptions = rocksdb_readoptions_create();
  rocksdb_readoptions_set_verify_checksums(roptions, 1);
  rocksdb_readoptions_set_fill_cache(roptions, 0);

  woptions = rocksdb_writeoptions_create();
  rocksdb_writeoptions_set_sync(woptions, 1);

  StartPhase("destroy");
  rocksdb_destroy_db(options, dbname, &err);
  Free(&err);

  StartPhase("open_error");
  db = rocksdb_open(options, dbname, &err);
  CheckCondition(err != NULL);
  Free(&err);

  StartPhase("open");
  rocksdb_options_set_create_if_missing(options, 1);
  db = rocksdb_open(options, dbname, &err);
  CheckNoError(err);
  CheckGet(db, roptions, "foo", NULL);

  StartPhase("put");
  rocksdb_put(db, woptions, "foo", 3, "hello", 5, &err);
  CheckNoError(err);
  CheckGet(db, roptions, "foo", "hello");

  StartPhase("compactall");
  rocksdb_compact_range(db, NULL, 0, NULL, 0);
  CheckGet(db, roptions, "foo", "hello");

  StartPhase("compactrange");
  rocksdb_compact_range(db, "a", 1, "z", 1);
  CheckGet(db, roptions, "foo", "hello");

  StartPhase("writebatch");
  {
    rocksdb_writebatch_t* wb = rocksdb_writebatch_create();
    rocksdb_writebatch_put(wb, "foo", 3, "a", 1);
    rocksdb_writebatch_clear(wb);
    rocksdb_writebatch_put(wb, "bar", 3, "b", 1);
    rocksdb_writebatch_put(wb, "box", 3, "c", 1);
    rocksdb_writebatch_delete(wb, "bar", 3);
    rocksdb_write(db, woptions, wb, &err);
    CheckNoError(err);
    CheckGet(db, roptions, "foo", "hello");
    CheckGet(db, roptions, "bar", NULL);
    CheckGet(db, roptions, "box", "c");
    int pos = 0;
    rocksdb_writebatch_iterate(wb, &pos, CheckPut, CheckDel);
    CheckCondition(pos == 3);
    rocksdb_writebatch_destroy(wb);
  }

  StartPhase("writebatch_rep");
  {
    rocksdb_writebatch_t* wb1 = rocksdb_writebatch_create();
    rocksdb_writebatch_put(wb1, "baz", 3, "d", 1);
    rocksdb_writebatch_put(wb1, "quux", 4, "e", 1);
    rocksdb_writebatch_delete(wb1, "quux", 4);
    size_t repsize1 = 0;
    const char* rep = rocksdb_writebatch_data(wb1, &repsize1);
    rocksdb_writebatch_t* wb2 = rocksdb_writebatch_create_from(rep, repsize1);
    CheckCondition(rocksdb_writebatch_count(wb1) ==
                   rocksdb_writebatch_count(wb2));
    size_t repsize2 = 0;
    CheckCondition(
        memcmp(rep, rocksdb_writebatch_data(wb2, &repsize2), repsize1) == 0);
    rocksdb_writebatch_destroy(wb1);
    rocksdb_writebatch_destroy(wb2);
  }

  StartPhase("iter");
  {
    rocksdb_iterator_t* iter = rocksdb_create_iterator(db, roptions);
    CheckCondition(!rocksdb_iter_valid(iter));
    rocksdb_iter_seek_to_first(iter);
    CheckCondition(rocksdb_iter_valid(iter));
    CheckIter(iter, "box", "c");
    rocksdb_iter_next(iter);
    CheckIter(iter, "foo", "hello");
    rocksdb_iter_prev(iter);
    CheckIter(iter, "box", "c");
    rocksdb_iter_prev(iter);
    CheckCondition(!rocksdb_iter_valid(iter));
    rocksdb_iter_seek_to_last(iter);
    CheckIter(iter, "foo", "hello");
    rocksdb_iter_seek(iter, "b", 1);
    CheckIter(iter, "box", "c");
    rocksdb_iter_get_error(iter, &err);
    CheckNoError(err);
    rocksdb_iter_destroy(iter);
  }

  StartPhase("approximate_sizes");
  {
    int i;
    int n = 20000;
    char keybuf[100];
    char valbuf[100];
    uint64_t sizes[2];
    const char* start[2] = { "a", "k00000000000000010000" };
    size_t start_len[2] = { 1, 21 };
    const char* limit[2] = { "k00000000000000010000", "z" };
    size_t limit_len[2] = { 21, 1 };
    rocksdb_writeoptions_set_sync(woptions, 0);
    for (i = 0; i < n; i++) {
      snprintf(keybuf, sizeof(keybuf), "k%020d", i);
      snprintf(valbuf, sizeof(valbuf), "v%020d", i);
      rocksdb_put(db, woptions, keybuf, strlen(keybuf), valbuf, strlen(valbuf),
                  &err);
      CheckNoError(err);
    }
    rocksdb_approximate_sizes(db, 2, start, start_len, limit, limit_len, sizes);
    CheckCondition(sizes[0] > 0);
    CheckCondition(sizes[1] > 0);
  }

  StartPhase("property");
  {
    char* prop = rocksdb_property_value(db, "nosuchprop");
    CheckCondition(prop == NULL);
    prop = rocksdb_property_value(db, "rocksdb.stats");
    CheckCondition(prop != NULL);
    Free(&prop);
  }

  StartPhase("snapshot");
  {
    const rocksdb_snapshot_t* snap;
    snap = rocksdb_create_snapshot(db);
    rocksdb_delete(db, woptions, "foo", 3, &err);
    CheckNoError(err);
    rocksdb_readoptions_set_snapshot(roptions, snap);
    CheckGet(db, roptions, "foo", "hello");
    rocksdb_readoptions_set_snapshot(roptions, NULL);
    CheckGet(db, roptions, "foo", NULL);
    rocksdb_release_snapshot(db, snap);
  }

  StartPhase("repair");
  {
    // If we do not compact here, then the lazy deletion of
    // files (https://reviews.facebook.net/D6123) would leave
    // around deleted files and the repair process will find
    // those files and put them back into the database.
    rocksdb_compact_range(db, NULL, 0, NULL, 0);
    rocksdb_close(db);
    rocksdb_options_set_create_if_missing(options, 0);
    rocksdb_options_set_error_if_exists(options, 0);
    rocksdb_repair_db(options, dbname, &err);
    CheckNoError(err);
    db = rocksdb_open(options, dbname, &err);
    CheckNoError(err);
    CheckGet(db, roptions, "foo", NULL);
    CheckGet(db, roptions, "bar", NULL);
    CheckGet(db, roptions, "box", "c");
    rocksdb_options_set_create_if_missing(options, 1);
    rocksdb_options_set_error_if_exists(options, 1);
  }

  StartPhase("filter");
  for (run = 0; run < 2; run++) {
    // First run uses custom filter, second run uses bloom filter
    CheckNoError(err);
    rocksdb_filterpolicy_t* policy;
    if (run == 0) {
      policy = rocksdb_filterpolicy_create(
          NULL, FilterDestroy, FilterCreate, FilterKeyMatch, NULL, FilterName);
    } else {
      policy = rocksdb_filterpolicy_create_bloom(10);
    }

    rocksdb_block_based_options_set_filter_policy(table_options, policy);

    // Create new database
    rocksdb_close(db);
    rocksdb_destroy_db(options, dbname, &err);
    rocksdb_options_set_block_based_table_factory(options, table_options);
    db = rocksdb_open(options, dbname, &err);
    CheckNoError(err);
    rocksdb_put(db, woptions, "foo", 3, "foovalue", 8, &err);
    CheckNoError(err);
    rocksdb_put(db, woptions, "bar", 3, "barvalue", 8, &err);
    CheckNoError(err);
    rocksdb_compact_range(db, NULL, 0, NULL, 0);

    fake_filter_result = 1;
    CheckGet(db, roptions, "foo", "foovalue");
    CheckGet(db, roptions, "bar", "barvalue");
    if (phase == 0) {
      // Must not find value when custom filter returns false
      fake_filter_result = 0;
      CheckGet(db, roptions, "foo", NULL);
      CheckGet(db, roptions, "bar", NULL);
      fake_filter_result = 1;

      CheckGet(db, roptions, "foo", "foovalue");
      CheckGet(db, roptions, "bar", "barvalue");
    }
    // Reset the policy
    rocksdb_block_based_options_set_filter_policy(table_options, NULL);
    rocksdb_options_set_block_based_table_factory(options, table_options);
  }

  StartPhase("compaction_filter");
  {
    rocksdb_options_t* options_with_filter = rocksdb_options_create();
    rocksdb_options_set_create_if_missing(options_with_filter, 1);
    rocksdb_compactionfilter_t* cfilter;
    cfilter = rocksdb_compactionfilter_create(NULL, CFilterDestroy,
                                              CFilterFilter, CFilterName);
    // Create new database
    rocksdb_close(db);
    rocksdb_destroy_db(options_with_filter, dbname, &err);
    rocksdb_options_set_compaction_filter(options_with_filter, cfilter);
    db = CheckCompaction(db, options_with_filter, roptions, woptions);

    rocksdb_options_set_compaction_filter(options_with_filter, NULL);
    rocksdb_compactionfilter_destroy(cfilter);
    rocksdb_options_destroy(options_with_filter);
  }

  StartPhase("compaction_filter_factory");
  {
    rocksdb_options_t* options_with_filter_factory = rocksdb_options_create();
    rocksdb_options_set_create_if_missing(options_with_filter_factory, 1);
    rocksdb_compactionfilterfactory_t* factory;
    factory = rocksdb_compactionfilterfactory_create(
        NULL, CFilterFactoryDestroy, CFilterCreate, CFilterFactoryName);
    // Create new database
    rocksdb_close(db);
    rocksdb_destroy_db(options_with_filter_factory, dbname, &err);
    rocksdb_options_set_compaction_filter_factory(options_with_filter_factory,
                                                  factory);
    db = CheckCompaction(db, options_with_filter_factory, roptions, woptions);

    rocksdb_options_set_compaction_filter_factory(
        options_with_filter_factory, NULL);
    rocksdb_options_destroy(options_with_filter_factory);
  }

  StartPhase("compaction_filter_v2");
  {
    rocksdb_compactionfilterfactoryv2_t* factory;
    rocksdb_slicetransform_t* prefix_extractor;
    prefix_extractor = rocksdb_slicetransform_create(
        NULL, CFV2PrefixExtractorDestroy, CFV2PrefixExtractorTransform,
        CFV2PrefixExtractorInDomain, CFV2PrefixExtractorInRange,
        CFV2PrefixExtractorName);
    factory = rocksdb_compactionfilterfactoryv2_create(
        prefix_extractor, prefix_extractor, CompactionFilterFactoryV2Destroy,
        CompactionFilterFactoryV2Create, CompactionFilterFactoryV2Name);
    // Create new database
    rocksdb_close(db);
    rocksdb_destroy_db(options, dbname, &err);
    rocksdb_options_set_compaction_filter_factory_v2(options, factory);
    db = rocksdb_open(options, dbname, &err);
    CheckNoError(err);
    // Only foo2 is GC'd, foo3 is changed.
    rocksdb_put(db, woptions, "foo1", 4, "no gc", 5, &err);
    CheckNoError(err);
    rocksdb_put(db, woptions, "foo2", 4, "gc", 2, &err);
    CheckNoError(err);
    rocksdb_put(db, woptions, "foo3", 4, "change", 6, &err);
    CheckNoError(err);
    // All bars are GC'd.
    rocksdb_put(db, woptions, "bar1", 4, "no gc", 5, &err);
    CheckNoError(err);
    rocksdb_put(db, woptions, "bar2", 4, "gc all", 6, &err);
    CheckNoError(err);
    rocksdb_put(db, woptions, "bar3", 4, "no gc", 5, &err);
    CheckNoError(err);
    // Compact the DB to garbage collect.
    rocksdb_compact_range(db, NULL, 0, NULL, 0);

    // Verify foo entries.
    CheckGet(db, roptions, "foo1", "no gc");
    CheckGet(db, roptions, "foo2", NULL);
    CheckGet(db, roptions, "foo3", "changed");
    // Verify bar entries were all deleted.
    CheckGet(db, roptions, "bar1", NULL);
    CheckGet(db, roptions, "bar2", NULL);
    CheckGet(db, roptions, "bar3", NULL);
  }

  StartPhase("merge_operator");
  {
    rocksdb_mergeoperator_t* merge_operator;
    merge_operator = rocksdb_mergeoperator_create(
        NULL, MergeOperatorDestroy, MergeOperatorFullMerge,
        MergeOperatorPartialMerge, NULL, MergeOperatorName);
    // Create new database
    rocksdb_close(db);
    rocksdb_destroy_db(options, dbname, &err);
    rocksdb_options_set_merge_operator(options, merge_operator);
    db = rocksdb_open(options, dbname, &err);
    CheckNoError(err);
    rocksdb_put(db, woptions, "foo", 3, "foovalue", 8, &err);
    CheckNoError(err);
    CheckGet(db, roptions, "foo", "foovalue");
    rocksdb_merge(db, woptions, "foo", 3, "barvalue", 8, &err);
    CheckNoError(err);
    CheckGet(db, roptions, "foo", "fake");

    // Merge of a non-existing value
    rocksdb_merge(db, woptions, "bar", 3, "barvalue", 8, &err);
    CheckNoError(err);
    CheckGet(db, roptions, "bar", "fake");

  }

  StartPhase("columnfamilies");
  {
    rocksdb_close(db);
    rocksdb_destroy_db(options, dbname, &err);
    CheckNoError(err)

    rocksdb_options_t* db_options = rocksdb_options_create();
    rocksdb_options_set_create_if_missing(db_options, 1);
    db = rocksdb_open(db_options, dbname, &err);
    CheckNoError(err)
    rocksdb_column_family_handle_t* cfh;
    cfh = rocksdb_create_column_family(db, db_options, "cf1", &err);
    rocksdb_column_family_handle_destroy(cfh);
    CheckNoError(err);
    rocksdb_close(db);

    size_t cflen;
    char** column_fams = rocksdb_list_column_families(db_options, dbname, &cflen, &err);
    CheckNoError(err);
    CheckEqual("default", column_fams[0], 7);
    CheckEqual("cf1", column_fams[1], 3);
    CheckCondition(cflen == 2);
    rocksdb_list_column_families_destroy(column_fams, cflen);

    rocksdb_options_t* cf_options = rocksdb_options_create();

    const char* cf_names[2] = {"default", "cf1"};
    const rocksdb_options_t* cf_opts[2] = {cf_options, cf_options};
    rocksdb_column_family_handle_t* handles[2];
    db = rocksdb_open_column_families(db_options, dbname, 2, cf_names, cf_opts, handles, &err);
    CheckNoError(err);

    rocksdb_put_cf(db, woptions, handles[1], "foo", 3, "hello", 5, &err);
    CheckNoError(err);

    CheckGetCF(db, roptions, handles[1], "foo", "hello");

    rocksdb_delete_cf(db, woptions, handles[1], "foo", 3, &err);
    CheckNoError(err);

    CheckGetCF(db, roptions, handles[1], "foo", NULL);

    rocksdb_writebatch_t* wb = rocksdb_writebatch_create();
    rocksdb_writebatch_put_cf(wb, handles[1], "baz", 3, "a", 1);
    rocksdb_writebatch_clear(wb);
    rocksdb_writebatch_put_cf(wb, handles[1], "bar", 3, "b", 1);
    rocksdb_writebatch_put_cf(wb, handles[1], "box", 3, "c", 1);
    rocksdb_writebatch_delete_cf(wb, handles[1], "bar", 3);
    rocksdb_write(db, woptions, wb, &err);
    CheckNoError(err);
    CheckGetCF(db, roptions, handles[1], "baz", NULL);
    CheckGetCF(db, roptions, handles[1], "bar", NULL);
    CheckGetCF(db, roptions, handles[1], "box", "c");
    rocksdb_writebatch_destroy(wb);

    rocksdb_iterator_t* iter = rocksdb_create_iterator_cf(db, roptions, handles[1]);
    CheckCondition(!rocksdb_iter_valid(iter));
    rocksdb_iter_seek_to_first(iter);
    CheckCondition(rocksdb_iter_valid(iter));

    int i;
    for (i = 0; rocksdb_iter_valid(iter) != 0; rocksdb_iter_next(iter)) {
      i++;
    }
    CheckCondition(i == 1);
    rocksdb_iter_get_error(iter, &err);
    CheckNoError(err);
    rocksdb_iter_destroy(iter);

    rocksdb_drop_column_family(db, handles[1], &err);
    CheckNoError(err);
    for (i = 0; i < 2; i++) {
      rocksdb_column_family_handle_destroy(handles[i]);
    }
    rocksdb_close(db);
    rocksdb_destroy_db(options, dbname, &err);
    rocksdb_options_destroy(db_options);
    rocksdb_options_destroy(cf_options);
  }

  StartPhase("prefix");
  {
    // Create new database
    rocksdb_options_set_allow_mmap_reads(options, 1);
    rocksdb_options_set_prefix_extractor(options, rocksdb_slicetransform_create_fixed_prefix(3));
    rocksdb_options_set_hash_skip_list_rep(options, 5000, 4, 4);
    rocksdb_options_set_plain_table_factory(options, 4, 10, 0.75, 16);

    db = rocksdb_open(options, dbname, &err);
    CheckNoError(err);

    rocksdb_put(db, woptions, "foo1", 4, "foo", 3, &err);
    CheckNoError(err);
    rocksdb_put(db, woptions, "foo2", 4, "foo", 3, &err);
    CheckNoError(err);
    rocksdb_put(db, woptions, "foo3", 4, "foo", 3, &err);
    CheckNoError(err);
    rocksdb_put(db, woptions, "bar1", 4, "bar", 3, &err);
    CheckNoError(err);
    rocksdb_put(db, woptions, "bar2", 4, "bar", 3, &err);
    CheckNoError(err);
    rocksdb_put(db, woptions, "bar3", 4, "bar", 3, &err);
    CheckNoError(err);

    rocksdb_iterator_t* iter = rocksdb_create_iterator(db, roptions);
    CheckCondition(!rocksdb_iter_valid(iter));

    rocksdb_iter_seek(iter, "bar", 3);
    rocksdb_iter_get_error(iter, &err);
    CheckNoError(err);
    CheckCondition(rocksdb_iter_valid(iter));

    CheckIter(iter, "bar1", "bar");
    rocksdb_iter_next(iter);
    CheckIter(iter, "bar2", "bar");
    rocksdb_iter_next(iter);
    CheckIter(iter, "bar3", "bar");
    rocksdb_iter_get_error(iter, &err);
    CheckNoError(err);
    rocksdb_iter_destroy(iter);
  }


  StartPhase("cleanup");
  rocksdb_close(db);
  rocksdb_options_destroy(options);
  rocksdb_block_based_options_destroy(table_options);
  rocksdb_readoptions_destroy(roptions);
  rocksdb_writeoptions_destroy(woptions);
  rocksdb_cache_destroy(cache);
  rocksdb_comparator_destroy(cmp);
  rocksdb_env_destroy(env);

  fprintf(stderr, "PASS\n");
  return 0;
}
