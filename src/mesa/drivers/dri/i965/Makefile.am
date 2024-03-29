

# Copyright © 2011 Intel Corporation
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

include Makefile.sources

AM_CFLAGS = \
	-I$(top_srcdir)/include \
	-I$(top_srcdir)/src/ \
	-I$(top_srcdir)/src/mapi \
	-I$(top_srcdir)/src/mesa/ \
	-I$(top_srcdir)/src/gallium/include \
	-I$(top_srcdir)/src/gallium/auxiliary \
	-I$(top_srcdir)/src/mesa/drivers/dri/common \
	-I$(top_srcdir)/src/mesa/drivers/dri/intel/server \
	-I$(top_srcdir)/src/gtest/include \
	-I$(top_srcdir)/src/compiler/nir \
	-I$(top_srcdir)/src/intel \
	-I$(top_builddir)/src/compiler/nir \
	-I$(top_builddir)/src/mesa/drivers/dri/common \
	$(DEFINES) \
	$(VISIBILITY_CFLAGS) \
	$(INTEL_CFLAGS)

AM_CXXFLAGS = $(AM_CFLAGS)

brw_nir_trig_workarounds.c: brw_nir_trig_workarounds.py $(top_srcdir)/src/compiler/nir/nir_algebraic.py
	$(MKDIR_GEN)
	$(AM_V_GEN) PYTHONPATH=$(top_srcdir)/src/compiler/nir $(PYTHON2) $(PYTHON_FLAGS) $(srcdir)/brw_nir_trig_workarounds.py > $@ || ($(RM) $@; false)

noinst_LTLIBRARIES = libi965_dri.la libi965_compiler.la
libi965_dri_la_SOURCES = $(i965_FILES)
libi965_dri_la_LIBADD = \
	$(top_builddir)/src/intel/isl/libisl.la \
	libi965_compiler.la \
	$(INTEL_LIBS)

libi965_compiler_la_SOURCES = \
	$(i965_compiler_FILES) \
	$(i965_compiler_GENERATED_FILES)

BUILT_SOURCES = $(i965_compiler_GENERATED_FILES)
CLEANFILES = $(BUILT_SOURCES)

TEST_LIBS = \
	libi965_compiler.la \
        ../../../libmesa.la \
	$(top_builddir)/src/intel/isl/libisl.la \
	$(PTHREAD_LIBS) \
	$(DLOPEN_LIBS) \
	../common/libdri_test_stubs.la

TESTS = \
	test_fs_cmod_propagation \
	test_fs_saturate_propagation \
        test_eu_compact \
	test_vf_float_conversions \
	test_vec4_cmod_propagation \
        test_vec4_copy_propagation \
        test_vec4_register_coalesce

check_PROGRAMS = $(TESTS)

test_fs_cmod_propagation_SOURCES = \
	test_fs_cmod_propagation.cpp
test_fs_cmod_propagation_LDADD = \
	$(top_builddir)/src/gtest/libgtest.la \
	$(TEST_LIBS)

test_fs_saturate_propagation_SOURCES = \
	test_fs_saturate_propagation.cpp
test_fs_saturate_propagation_LDADD = \
	$(top_builddir)/src/gtest/libgtest.la \
	$(TEST_LIBS)

test_vf_float_conversions_SOURCES = \
	test_vf_float_conversions.cpp
test_vf_float_conversions_LDADD = \
	$(top_builddir)/src/gtest/libgtest.la \
	$(TEST_LIBS)

test_vec4_register_coalesce_SOURCES = \
	test_vec4_register_coalesce.cpp
test_vec4_register_coalesce_LDADD = \
        $(top_builddir)/src/gtest/libgtest.la \
        $(TEST_LIBS)

test_vec4_copy_propagation_SOURCES = \
	test_vec4_copy_propagation.cpp
test_vec4_copy_propagation_LDADD = \
        $(top_builddir)/src/gtest/libgtest.la \
        $(TEST_LIBS)

test_vec4_cmod_propagation_SOURCES = \
	test_vec4_cmod_propagation.cpp
test_vec4_cmod_propagation_LDADD = \
	$(top_builddir)/src/gtest/libgtest.la \
	$(TEST_LIBS)

test_eu_compact_SOURCES = \
	test_eu_compact.c
nodist_EXTRA_test_eu_compact_SOURCES = dummy.cpp
test_eu_compact_LDADD = $(TEST_LIBS)
