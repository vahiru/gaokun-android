# Insert after the shebang in the reviewed installed host-role script.
# This also covers the system-sleep hook's direct invocation of that script.
gaokun3_role_cmdline=$(cat /proc/cmdline) || exit 1
case " $gaokun3_role_cmdline " in
    *" gaokun3.hos_rescue_device=1 "*)
        logger -t gaokun3-usb-role 'HOS rescue device-only boot: host request skipped'
        exit 0
        ;;
esac
unset gaokun3_role_cmdline
