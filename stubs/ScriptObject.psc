Scriptname ScriptObject Native
; Declarations only, for compiling. The game supplies the real script.
bool Function RegisterForRemoteEvent(ScriptObject akEventSource, ScriptEventName asEventName) native
Function UnregisterForRemoteEvent(ScriptObject akEventSource, ScriptEventName asEventName) native
Function StartTimer(float afInterval, int aiTimerID = 0) native
Function CancelTimer(int aiTimerID = 0) native
Event OnInit()
EndEvent
Event OnTimer(int aiTimerID)
EndEvent
Function AddInventoryEventFilter(Form akFilter) native
