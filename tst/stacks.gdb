set pagination off
set confirm off
set auto-load off
set disable-randomization off
set print thread-events off
set print inferior-events off
handle SIGPIPE nostop noprint pass
run
if !$_isvoid($_exitcode)
  quit $_exitcode
end
if !$_isvoid($_exitsignal)
  quit 128 + $_exitsignal
end
set $signal = $_siginfo.si_signo
thread apply all bt
bt
kill
quit 128 + $signal
