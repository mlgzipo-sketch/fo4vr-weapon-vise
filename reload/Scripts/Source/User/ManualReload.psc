Scriptname ManualReload Native
{Functions of ManualReload.dll (F4SE plugin) for the reload script.}

int Function PollCommand() global native
int Function CommandKind() global native
; Form ids, not forms: a form returned by a plugin function crashed the VM (CreateObject)
int Function CommandRefID() global native
int Function CommandToken() global native
float[] Function CommandPose() global native
; where a magazine for a hand goes now (the hand has moved since the command); empty if not for a hand
float[] Function FreshPose(int aiToken) global native
Function AddMag(ObjectReference akMag, int aiToken) global native
Function AddGhost(ObjectReference akGhost, int aiKind) global native
Function Notify(int aiMessage) global native
