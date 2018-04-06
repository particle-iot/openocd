#
# Check for installed gperf and its version
#

AC_DEFUN([AX_GPERF_VERSION],[
AC_CHECK_PROG([gperf],[gperf],[yes],[no])

AS_IF([test "x$gperf" = xno], [
  AC_MSG_WARN([gperf not found])
  AM_CONDITIONAL([HAVE_GPERF_VERSION], false)
])

AS_IF([test "x$gperf" = xyes], [
  AC_MSG_CHECKING([if gperf version is compatible])

  GPERF_VERSION=`gperf --version | head -n1 | cut -d' ' -f3`

  AX_COMPARE_VERSION([$GPERF_VERSION], [ge], [$1], [
    AC_MSG_RESULT([yes])
    AM_CONDITIONAL([HAVE_GPERF_VERSION], true)
  ], [
    AC_MSG_RESULT([no])
    AC_MSG_WARN([not using gperf because of version mismatch])
    AM_CONDITIONAL([HAVE_GPERF_VERSION], false)
  ])
])

])dnl AX_GPERF_VERSION
