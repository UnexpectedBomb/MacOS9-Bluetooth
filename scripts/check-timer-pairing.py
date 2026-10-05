#!/usr/bin/env python3
"""check-timer-pairing.py -- every armed BTstack timer must also be ADDED to the run loop.

WHY THIS EXISTS. Driver 15.0 shipped a connect sweep that never ran. It called
btstack_run_loop_set_timer_handler() and btstack_run_loop_set_timer(), but not
btstack_run_loop_add_timer(). set_timer only computes the expiry into the timer struct;
add_timer is what links it into the run loop's list. Nothing warns, nothing fails, and
nothing logs: the callback is simply never called. The symptom was a counter reading 0 on
a run loop that had turned 2061 times, and it cost a hardware boot to notice.

Four other timers in the same file did it correctly on the line below. A count of calls
would have said "5 and 4, close enough"; what matters is WHICH timer, so this matches per
timer variable.

⚠ A timer that is only ever removed (btstack_run_loop_remove_timer) is fine and ignored.
"""
import re
import sys
import pathlib

SRC = pathlib.Path(__file__).resolve().parent.parent / "src"

SET = re.compile(r"btstack_run_loop_set_timer\s*\(\s*&\s*(\w+)")
ADD = re.compile(r"btstack_run_loop_add_timer\s*\(\s*&\s*(\w+)")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def main():
    failures = []
    checked = 0
    for path in sorted(SRC.glob("*.c")):
        body = strip_comments(path.read_text(encoding="utf-8"))
        armed = set(SET.findall(body))
        added = set(ADD.findall(body))
        checked += len(armed)
        for timer in sorted(armed - added):
            failures.append(f"  {path.name}: {timer} is set_timer'd but never add_timer'd")

    if failures:
        sys.stderr.write(
            "TIMER NEVER ADDED TO THE RUN LOOP -- its callback will never fire, "
            "and nothing will say so:\n" + "\n".join(failures) + "\n"
        )
        return 1

    print(f"   [ok] timers: {checked} armed timer(s), every one added to the run loop")
    return 0


if __name__ == "__main__":
    sys.exit(main())
