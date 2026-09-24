/** Starter scripts for the Scripts tab. Same API as Logitech G Hub. */
export interface ScriptExample {
  id: string;
  name: string;
  source: string;
}

export const SCRIPT_EXAMPLES: ScriptExample[] = [
  {
    id: "hold-move",
    name: "Hold back button → move left",
    source: `-- While the back side button (4) is held, glide the cursor left.
-- SetMouseButtonBlocked keeps the button itself from reaching the PC,
-- so the browser doesn't also go "Back".

function OnEvent(event, arg)
  if event == "PROFILE_ACTIVATED" then
    SetMouseButtonBlocked(4, true)
    print("ready: hold the back button")
  elseif event == "MOUSE_BUTTON_PRESSED" and arg == 4 then
    repeat
      MoveMouseRelative(-5, 0)
      Sleep(10)
    until not IsMouseButtonPressed(4)
  end
end
`,
  },
  {
    id: "rapid-click",
    name: "Hold forward button → rapid left click",
    source: `-- While the forward side button (5) is held, click left every 50 ms.

function OnEvent(event, arg)
  if event == "PROFILE_ACTIVATED" then
    SetMouseButtonBlocked(5, true)
  elseif event == "MOUSE_BUTTON_PRESSED" and arg == 5 then
    repeat
      PressAndReleaseMouseButton(1)
      Sleep(50)
    until not IsMouseButtonPressed(5)
  end
end
`,
  },
  {
    id: "log-events",
    name: "Log every button event",
    source: `-- Prints each event so you can see which number each button has.
-- (Left click is only reported after EnablePrimaryMouseButtonEvents(true).)

EnablePrimaryMouseButtonEvents(true)

function OnEvent(event, arg, family)
  OutputLogMessage("%6d ms  %s  arg=%d  %s\\n", GetRunningTime(), event, arg, family)
end
`,
  },
];

/** One line per API function, shown in the reference card. */
export const SCRIPT_API: { sig: string; doc: string }[] = [
  { sig: "OnEvent(event, arg, family)", doc: 'You define this. event: "PROFILE_ACTIVATED", "PROFILE_DEACTIVATED", "MOUSE_BUTTON_PRESSED", "MOUSE_BUTTON_RELEASED". arg: button 1 left, 2 right, 3 middle, 4 back, 5 forward.' },
  { sig: "MoveMouseRelative(dx, dy)", doc: "Move the cursor by dx, dy counts." },
  { sig: "MoveMouseWheel(n)", doc: "Scroll n notches (positive = up)." },
  { sig: "PressMouseButton(n) / ReleaseMouseButton(n)", doc: "Hold / let go of a button. n: 1 left, 2 middle, 3 right, 4 back, 5 forward (G Hub's order)." },
  { sig: "PressAndReleaseMouseButton(n)", doc: "One click." },
  { sig: "IsMouseButtonPressed(n)", doc: "Is the physical button held? Same numbering as PressMouseButton." },
  { sig: "Sleep(ms)", doc: "Wait. Button events queue up meanwhile, like in G Hub." },
  { sig: "GetRunningTime()", doc: "Milliseconds since the script started." },
  { sig: "OutputLogMessage(fmt, ...) / print(...)", doc: "Write to the output pane (string.format rules)." },
  { sig: "ClearLog()", doc: "Clear the output pane." },
  { sig: "EnablePrimaryMouseButtonEvents(on)", doc: "Also send OnEvent for the left button (off by default)." },
  { sig: "SetMouseButtonBlocked(n, on)", doc: "Not in G Hub: hide a physical button from the PC while the script runs. n as in PressMouseButton." },
];
