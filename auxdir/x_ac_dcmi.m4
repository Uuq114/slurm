##*****************************************************************************
#  SYNOPSIS:
#    X_AC_DCMI
#
#  DESCRIPTION:
#    Determine if Huawei Ascend NPU DCMI API library exists.
##*****************************************************************************

AC_DEFUN([X_AC_DCMI],
[
  AC_ARG_WITH(
    [dcmi],
    AS_HELP_STRING([--without-dcmi], [Do not build Huawei Ascend NPU DCMI-related code]),
    []
  )

  if test "x$with_dcmi" = xno; then
    AC_MSG_WARN([support for dcmi disabled])
    ac_dcmi_h=no
    ac_dcmi_lib=no
  else
    # 保存原始编译标志
    cppflags_save="$CPPFLAGS"
    
    # 设置 DCMI 特定的头文件路径 - 包含两个路径
    DCMI_CPPFLAGS="-I/usr/local/dcmi -I/usr/local/Ascend/driver/kernel/inc/driver"
    
    AC_MSG_CHECKING([for dcmi_interface_api.h])
    CPPFLAGS="$DCMI_CPPFLAGS $CPPFLAGS"
    AC_COMPILE_IFELSE(
      [AC_LANG_PROGRAM([[#include <dcmi_interface_api.h>]], [[]])],
      [ac_dcmi_h=yes],
      [ac_dcmi_h=no]
    )
    CPPFLAGS="$cppflags_save"
    AC_MSG_RESULT([$ac_dcmi_h])
    
    if test "$ac_dcmi_h" = "yes"; then
      AC_MSG_CHECKING([for DCMI library])
      
      if test -f "/usr/local/dcmi/lib/libdcmi.so"; then
        DCMI_CPPFLAGS="$DCMI_CPPFLAGS -I/usr/local/dcmi/include"
        DCMI_LDFLAGS="-L/usr/local/dcmi/lib"
        DCMI_LIBS="-ldcmi"
        ac_dcmi_lib=yes
        AC_MSG_RESULT([yes])
      elif test -f "/usr/local/Ascend/driver/lib64/driver/libdrvdsmi_host.so"; then
        DCMI_LDFLAGS="-L/usr/local/Ascend/driver/lib64/driver"
        DCMI_LIBS="-ldrvdsmi_host"
        ac_dcmi_lib=yes
        AC_MSG_RESULT([yes])
      else
        ac_dcmi_lib=no
        AC_MSG_RESULT([no])
        AC_MSG_WARN([DCMI library not found in any known location])
      fi
      
      if test "$ac_dcmi_lib" = "yes"; then
        AC_DEFINE([HAVE_DCMI], [1], [Define to 1 if DCMI library found])
        AC_MSG_NOTICE([DCMI support enabled])
      fi
    else
      AC_MSG_WARN([DCMI header not found in /usr/local/dcmi or /usr/local/Ascend/driver/kernel/inc/driver])
      ac_dcmi_lib=no
    fi
    
    AC_SUBST([DCMI_CPPFLAGS])
    AC_SUBST([DCMI_LIBS])
    AC_SUBST([DCMI_LDFLAGS])  # 👈 新增这一行！
  fi
  AM_CONDITIONAL([BUILD_DCMI], [test "$ac_dcmi_h" = "yes" -a "$ac_dcmi_lib" = "yes"])
])