#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for optimizer/util
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/optimizer/util/Makefile,v 1.18 2008/02/19 10:30:07 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/optimizer/util
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
override CPPFLAGS := -I$../../gp_libpq_fe $(CPPFLAGS)

OBJS = clauses.o joininfo.o pathnode.o placeholder.o plancat.o predtest.o \
       relnode.o restrictinfo.o tlist.o var.o walkers.o

include $(top_srcdir)/src/backend/common.mk
