/* Regression test for DBusVariant
 *
 * Copyright © 2017 Collabora Ltd.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <config.h>

#include <string.h>

#include <glib.h>
#include <glib-object.h>

#include <dbus/dbus.h>
#include <dbus/dbus-internals.h>
#include <dbus/dbus-string.h>
#include <dbus/dbus-message-internal.h>
#include "test-utils-glib.h"

typedef struct
{
  DBusMessage *original;
  DBusMessage *copy;
} Fixture;

static void oom (void) G_GNUC_NORETURN;
static void
oom (void)
{
  g_error ("Out of memory");
}

static void
setup (Fixture *f,
       gconstpointer data)
{
  dbus_int32_t fortytwo = 42;
  dbus_int64_t twentythree = 23;
  const char *s = "Hello, world!";
  DBusMessageIter iter;
  DBusMessageIter arr_iter;
  DBusMessageIter struct_iter;
  DBusMessageIter pair_iter;

  f->original = dbus_message_new_signal ("/", "a.b", "c");

  /* It ends up as:
   * (
   *  int32 42,
   *  "Hello, world!",
   *  int64 23,
   *  [int32 42, int32 42],
   *  (int32 42, "Hello, world!", int64 23),
   *  {int32 42: int64 23},
   * )
   */

  if (!dbus_message_append_args (f->original,
                                 DBUS_TYPE_INT32, &fortytwo,
                                 DBUS_TYPE_STRING, &s,
                                 DBUS_TYPE_INT64, &twentythree,
                                 DBUS_TYPE_INVALID))
    oom ();

  dbus_message_iter_init_append (f->original, &iter);

  if (!dbus_message_iter_open_container (&iter, DBUS_TYPE_ARRAY,
                                         DBUS_TYPE_INT32_AS_STRING, &arr_iter))
    oom ();

  {
    if (!dbus_message_iter_append_basic (&arr_iter, DBUS_TYPE_INT32, &fortytwo))
      oom ();

    if (!dbus_message_iter_append_basic (&arr_iter, DBUS_TYPE_INT32, &fortytwo))
      oom ();
  }

  if (!dbus_message_iter_close_container (&iter, &arr_iter))
    oom ();

  if (!dbus_message_iter_open_container (&iter, DBUS_TYPE_STRUCT, NULL,
                                         &struct_iter))
    oom ();

  {
    if (!dbus_message_iter_append_basic (&struct_iter, DBUS_TYPE_INT32,
                                         &fortytwo))
      oom ();

    if (!dbus_message_iter_append_basic (&struct_iter, DBUS_TYPE_STRING, &s))
      oom ();

    if (!dbus_message_iter_append_basic (&struct_iter, DBUS_TYPE_INT64,
                                         &twentythree))
      oom ();
  }

  if (!dbus_message_iter_close_container (&iter, &struct_iter))
    oom ();

  if (!dbus_message_iter_open_container (&iter, DBUS_TYPE_ARRAY, "{ix}",
                                         &arr_iter))
    oom ();

  {
    if (!dbus_message_iter_open_container (&arr_iter, DBUS_TYPE_DICT_ENTRY,
                                           NULL, &pair_iter))
      oom ();

    {
      if (!dbus_message_iter_append_basic (&pair_iter, DBUS_TYPE_INT32,
                                           &fortytwo))
        oom ();

      if (!dbus_message_iter_append_basic (&pair_iter, DBUS_TYPE_INT64,
                                           &twentythree))
        oom ();
    }

    if (!dbus_message_iter_close_container (&arr_iter, &pair_iter))
      oom ();
  }

  if (!dbus_message_iter_close_container (&iter, &arr_iter))
    oom ();
}

/* Assert that item_iter points to an int32 equal to expected_value.
 * Copy it into a DBusVariant and assert that the copy is done correctly. */
static void
assert_int32 (DBusMessageIter *item_iter,
              dbus_int32_t expected_value)
{
  DBusVariant *v;
  const DBusString *s;
  const void *value_p;
  dbus_int32_t value;

  g_assert_cmpint (dbus_message_iter_get_arg_type (item_iter), ==,
                   DBUS_TYPE_INT32);
  dbus_message_iter_get_basic (item_iter, &value);
  g_assert_cmpint (value, ==, expected_value);

  v = _dbus_variant_read (item_iter);
  g_assert (v != NULL);
  s = _dbus_variant_peek (v);
  g_assert (s != NULL);
  g_assert_cmpstr (_dbus_variant_get_signature (v), ==,
                   DBUS_TYPE_INT32_AS_STRING);

  /* Variant serialization of <int32 something> at offset 0:
   * 01 'i' 00                  signature
   *           00               padding
   *               vv vv vv vv  bytes of value
   */
  g_assert_cmpint (_dbus_string_get_length (s), ==, 8);
  g_assert_cmpint (_dbus_string_get_byte (s, 0), ==, 1);
  g_assert_cmpint (_dbus_string_get_byte (s, 1), ==, DBUS_TYPE_INT32);
  g_assert_cmpint (_dbus_string_get_byte (s, 2), ==, '\0');
  g_assert_cmpint (_dbus_string_get_byte (s, 3), ==, 0); /* padding */
  value_p = _dbus_string_get_const_data_len (s, 4, 4);
  memcpy (&value, value_p, 4);
  g_assert_cmpint (value, ==, expected_value);

  _dbus_variant_free (v);
}

/* Assert that item_iter points to an int64 equal to expected_value.
 * Copy it into a DBusVariant and assert that the copy is done correctly. */
static void
assert_int64 (DBusMessageIter *item_iter,
              dbus_int64_t expected_value)
{
  DBusVariant *v;
  const DBusString *s;
  const void *value_p;
  dbus_int64_t value;
  int i;

  g_assert_cmpint (dbus_message_iter_get_arg_type (item_iter), ==,
                   DBUS_TYPE_INT64);
  dbus_message_iter_get_basic (item_iter, &value);
  g_assert_cmpint (value, ==, expected_value);

  v = _dbus_variant_read (item_iter);
  g_assert (v != NULL);
  s = _dbus_variant_peek (v);
  g_assert (s != NULL);
  g_assert_cmpstr (_dbus_variant_get_signature (v), ==,
                   DBUS_TYPE_INT64_AS_STRING);

  /* Variant serialization of <int64 something> at offset 0:
   * 01 'i' 00                  signature
   *          00  00 00 00 00  padding
   * vv vv vv vv  vv vv vv vv  bytes of value
   */
  g_assert_cmpint (_dbus_string_get_length (s), ==, 16);
  g_assert_cmpint (_dbus_string_get_byte (s, 0), ==, 1);
  g_assert_cmpint (_dbus_string_get_byte (s, 1), ==, DBUS_TYPE_INT64);
  g_assert_cmpint (_dbus_string_get_byte (s, 2), ==, '\0');

  for (i = 3; i < 8; i++)
    g_assert_cmpint (_dbus_string_get_byte (s, i), ==, 0); /* padding */

  value_p = _dbus_string_get_const_data_len (s, 8, 8);
  memcpy (&value, value_p, 8);
  g_assert_cmpint (value, ==, expected_value);

  _dbus_variant_free (v);
}

/* Assert that item_iter points to a string equal to expected_value.
 * Copy it into a DBusVariant and assert that the copy is done correctly. */
static void
assert_string (DBusMessageIter *item_iter,
               const char *expected_value)
{
  DBusVariant *v;
  const DBusString *s;
  const char *value;
  dbus_int32_t length;

  g_assert_cmpint (dbus_message_iter_get_arg_type (item_iter), ==,
                   DBUS_TYPE_STRING);
  dbus_message_iter_get_basic (item_iter, &value);
  g_assert_cmpstr (value, ==, expected_value);

  v = _dbus_variant_read (item_iter);
  g_assert (v != NULL);
  s = _dbus_variant_peek (v);
  g_assert (s != NULL);
  g_assert_cmpstr (_dbus_variant_get_signature (v), ==,
                   DBUS_TYPE_STRING_AS_STRING);

  /* Variant serialization of <"something"> at offset 0:
   * 01 's' 00                  signature
   *          00                padding
   *              ll ll ll ll   bytes of length excluding \0
   * vv vv vv ... 00            bytes of value
   */
  g_assert_cmpint (_dbus_string_get_length (s), ==,
                   (int) strlen (expected_value) + 9);
  g_assert_cmpint (_dbus_string_get_byte (s, 0), ==, 1);
  g_assert_cmpint (_dbus_string_get_byte (s, 1), ==, DBUS_TYPE_STRING);
  g_assert_cmpint (_dbus_string_get_byte (s, 2), ==, '\0');
  g_assert_cmpint (_dbus_string_get_byte (s, 3), ==, 0); /* padding */

  value = _dbus_string_get_const_data_len (s, 4, 4);
  memcpy (&length, value, 4);
  g_assert_cmpuint (length, ==, (int) strlen (expected_value));
  value = _dbus_string_get_const_data_len (s, 8, length + 1);
  g_assert_cmpstr (value, ==, expected_value);
}

/* Assert that item_iter points to an array of n_values repetitions of the
 * int32 expected_value. Copy it into a DBusVariant and assert that the
 * copy is done correctly. */
static void
assert_array_of_int32 (DBusMessageIter *item_iter,
                       int n_values,
                       dbus_int32_t expected_value)
{
  DBusMessageIter arr_iter;
  DBusVariant *v;
  const DBusString *s;
  const void *value_p;
  dbus_int32_t value;
  int i;

  g_assert_cmpint (dbus_message_iter_get_arg_type (item_iter), ==,
                   DBUS_TYPE_ARRAY);

  dbus_message_iter_recurse (item_iter, &arr_iter);

  for (i = 0; i < n_values; i++)
    {
      assert_int32 (&arr_iter, expected_value);

      if (i == n_values - 1)
        g_assert_false (dbus_message_iter_next (&arr_iter));
      else
        g_assert_true (dbus_message_iter_next (&arr_iter));
    }

  v = _dbus_variant_read (item_iter);
  g_assert (v != NULL);
  s = _dbus_variant_peek (v);
  g_assert (s != NULL);
  g_assert_cmpstr (_dbus_variant_get_signature (v), ==,
                   DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_INT32_AS_STRING);

  /* Variant serialization of <[int32 something, ...]> at offset 0:
   * 02 'a' 'i' 00              signature
   *               ll ll ll ll  total number of bytes in values
   * vv vv vv vv   ...          bytes of values
   */
  g_assert_cmpint (_dbus_string_get_length (s), ==, 8 + (4 * n_values));
  g_assert_cmpint (_dbus_string_get_byte (s, 0), ==, 2);
  g_assert_cmpint (_dbus_string_get_byte (s, 1), ==, DBUS_TYPE_ARRAY);
  g_assert_cmpint (_dbus_string_get_byte (s, 2), ==, DBUS_TYPE_INT32);
  g_assert_cmpint (_dbus_string_get_byte (s, 3), ==, '\0');
  value_p = _dbus_string_get_const_data_len (s, 4, 4);
  memcpy (&value, value_p, 4);
  g_assert_cmpint (value, ==, n_values * 4);

  for (i = 0; i < n_values; i++)
    {
      value_p = _dbus_string_get_const_data_len (s, 8 + (4 * (n_values - 1)),
                                                 4);
      memcpy (&value, value_p, 4);
      g_assert_cmpint (value, ==, expected_value);
    }

  _dbus_variant_free (v);
}

/* Assert that m is (in GVariant notation):
 * (
 *  int32 42,
 *  "Hello, world!",
 *  int64 23,
 *  [int32 42, int32 42],
 *  (int32 42, "Hello, world!", int64 23],
 *  {int32 42: int64 23},
 * )
 *
 * Serialize some of those values into DBusVariants and assert that it is
 * done correctly.
 */
static void
assert_message_as_expected (DBusMessage *m)
{
  DBusMessageIter item_iter;
  DBusMessageIter arr_iter;
  DBusMessageIter struct_iter;
  DBusMessageIter pair_iter;

  g_assert_cmpstr (dbus_message_get_signature (m), ==, "isxai(isx)a{ix}");
  dbus_message_iter_init (m, &item_iter);

  {
    assert_int32 (&item_iter, 42);
    g_assert_true (dbus_message_iter_next (&item_iter));

    assert_string (&item_iter, "Hello, world!");
    g_assert_true (dbus_message_iter_next (&item_iter));

    assert_int64 (&item_iter, 23);
    g_assert_true (dbus_message_iter_next (&item_iter));

    g_assert_cmpint (dbus_message_iter_get_arg_type (&item_iter), ==,
                     DBUS_TYPE_ARRAY);
    assert_array_of_int32 (&item_iter, 2, 42);

    g_assert_true (dbus_message_iter_next (&item_iter));

    g_assert_cmpint (dbus_message_iter_get_arg_type (&item_iter), ==,
                     DBUS_TYPE_STRUCT);
    dbus_message_iter_recurse (&item_iter, &struct_iter);

    {
      assert_int32 (&struct_iter, 42);
      g_assert_true (dbus_message_iter_next (&struct_iter));
      assert_string (&struct_iter, "Hello, world!");
      g_assert_true (dbus_message_iter_next (&struct_iter));
      assert_int64 (&struct_iter, 23);
      g_assert_false (dbus_message_iter_next (&struct_iter));
    }

    g_assert_true (dbus_message_iter_next (&item_iter));

    g_assert_cmpint (dbus_message_iter_get_arg_type (&item_iter), ==,
                     DBUS_TYPE_ARRAY);
    dbus_message_iter_recurse (&item_iter, &arr_iter);

    {
      g_assert_cmpint (dbus_message_iter_get_arg_type (&arr_iter), ==,
                       DBUS_TYPE_DICT_ENTRY);
      dbus_message_iter_recurse (&arr_iter, &pair_iter);

      {
        assert_int32 (&pair_iter, 42);
        g_assert_true (dbus_message_iter_next (&pair_iter));
        assert_int64 (&pair_iter, 23);
        g_assert_false (dbus_message_iter_next (&pair_iter));
      }

      g_assert_false (dbus_message_iter_next (&arr_iter));
    }
  }

  g_assert_false (dbus_message_iter_next (&item_iter));
}

static void
test (Fixture *f,
      gconstpointer data)
{
  DBusMessageIter item_iter;
  DBusMessageIter appender;
  int i;

  assert_message_as_expected (f->original);
  dbus_message_iter_init (f->original, &item_iter);

  f->copy = dbus_message_new_signal ("/", "a.b", "c");

  dbus_message_iter_init_append (f->copy, &appender);

  for (i = 0; i < 6; i++)
    {
      DBusVariant *var = _dbus_variant_read (&item_iter);

      _dbus_variant_write (var, &appender);
      _dbus_variant_free (var);

      if (i == 5)
        g_assert_false (dbus_message_iter_next (&item_iter));
      else
        g_assert_true (dbus_message_iter_next (&item_iter));
    }

  assert_message_as_expected (f->copy);
}

static void
teardown (Fixture *f,
          gconstpointer data)
{
  if (f->original)
    dbus_message_unref (f->original);

  if (f->copy)
    dbus_message_unref (f->copy);
}

int
main (int argc,
      char **argv)
{
  test_init (&argc, &argv);

  g_test_add ("/variant", Fixture, NULL, setup, test, teardown);

  return g_test_run ();
}
