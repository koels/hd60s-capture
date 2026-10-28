#!/bin/sh
# After an update, every piece should be on the same version. Prints each one
# and exits 1 if any disagree, so it can follow `make bundle` in a script.
cd "$(dirname "$0")/.."
want=$(cat VERSION)
build=$(awk -F. '{printf "%d",$1*10000+$2*100+$3}' VERSION)
root=/tmp/hd60s-$(id -u)
plist() { [ -f "$1" ] && /usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$1" 2>/dev/null || echo missing; }
# The installed copy is what users run and what updates replace; a fresh build
# is only checked when nothing is installed.
app="/Applications/HD60 S Capture.app"
[ -d "$app" ] || app="build/HD60 S Capture.app"
bad=0
row() { if [ "$2" = "$3" ]; then mark=ok; else mark=MISMATCH; bad=1; fi; printf "%-22s %-10s %s\n" "$1" "$2" "$mark"; }
echo "VERSION file          $want (build $build)"
echo "checking              $app"
row "app bundle"          "$(plist "$app/Contents/Info.plist")" "$want"
row "plugin in app"       "$(plist "$app/Contents/Resources/hd60s-native.plugin/Contents/Info.plist")" "$want"
row "plugin in OBS"       "$(plist "$HOME/Library/Application Support/obs-studio/plugins/hd60s-native.plugin/Contents/Info.plist")" "$want"
row "running service"     "$(cut -d' ' -f1 "$root/service.id" 2>/dev/null || echo none)" "$build"
row "service floor"       "$(cat "$root/min-build" 2>/dev/null || echo none)" "$build"
pid=$(cut -d' ' -f2 "$root/service.id" 2>/dev/null)
if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then echo "service pid $pid       alive"; else echo "service pid ${pid:-none}       NOT RUNNING"; bad=1; fi
exit $bad
