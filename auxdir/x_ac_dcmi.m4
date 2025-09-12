##*****************************************************************************
#  SYNOPSIS:
#    X_AC_DCMI
#
#  DESCRIPTION:
#    Determine if Huawei Ascend NPU DCMI API library exists.
##*****************************************************************************

AC_DEFUN([X_AC_DCMI],
[
  func_check_path ()
  {
      AS_UNSET([ac_cv_header_dsmi_common_interface_h])
      AS_UNSET([ac_cv_lib_drvdsmi_host_dsmi_init])
      AC_CHECK_HEADER([dsmi_common_interface.h], [ac_dcmi_h=yes], [ac_dcmi_h=no])
      AC_CHECK_LIB([drvdsmi_host], [dsmi_init], [ac_dcmi=yes], [ac_dcmi=no])
  }

  _x_ac_dcmi_dirs="/usr/local/Ascend/driver"
  _x_ac_dcmi_libs="lib64/stub"

  AC_ARG_WITH(
    [dcmi],
    AS_HELP_STRING(--with-dcmi=PATH, Specify path to Ascend DCMI installation),
    [AS_IF([test "x$with_dcmi" != xno && test "x$with_dcmi" != xyes],
           [_x_ac_dcmi_dirs="$with_dcmi"])])

  if [test "x$with_dcmi" = xno]; then
     AC_MSG_NOTICE([support for dcmi disabled])
  else

    # Check if libdrvdsmi_host is already in the system paths
    func_check_path

    if [ test "$ac_dcmi" = "yes" && test "$ac_dcmi_h" = "yes" ]; then
          # found in system path
          dcmi_includes="-I/usr/local/Ascend/driver/include"
          dcmi_libs="-ldrvdsmi_host"
    else
      # Try to find libdrvdsmi_host
      for d in $_x_ac_dcmi_dirs; do
        if [ ! test -d "$d" ]; then
          continue
        fi
        for bit in $_x_ac_dcmi_libs; do
          if [ ! test -d "$d/$bit" || ! test -d "$d/include" ]; then
            continue
          fi
          _x_ac_dcmi_ldflags_save="$LDFLAGS"
          _x_ac_dcmi_cppflags_save="$CPPFLAGS"
          LDFLAGS="-L$d/$bit -ldrvdsmi_host"
          CPPFLAGS="-I$d/include $CPPFLAGS"

          func_check_path

          LDFLAGS="$_x_ac_dcmi_ldflags_save"
          CPPFLAGS="$_x_ac_dcmi_cppflags_save"
          if [ test "$ac_dcmi" = "yes" && test "$ac_dcmi_h" = "yes" ]; then
            dcmi_includes="-I$d/include"
            break
          fi
        done
        if [ test "$ac_dcmi" = "yes" && test "$ac_dcmi_h" = "yes" ]; then
          break
        fi
      done
    fi

    if [ test "$ac_dcmi" = "yes" && test "$ac_dcmi_h" = "yes" ]; then
      DCMI_CPPFLAGS="$dcmi_includes"
      AC_DEFINE(HAVE_DCMI, 1, [Define to 1 if DCMI library found])
    else
      if test -z "$with_dcmi"; then
        AC_MSG_WARN([unable to locate libdrvdsmi_host.so and/or dsmi_common_interface.h])
      else
        AC_MSG_ERROR([unable to locate libdrvdsmi_host.so and/or dsmi_common_interface.h])
      fi
    fi

    AC_SUBST(DCMI_CPPFLAGS)
  fi
  AM_CONDITIONAL(BUILD_DCMI, test "$ac_dcmi" = "yes" && test "$ac_dcmi_h" = "yes")
])