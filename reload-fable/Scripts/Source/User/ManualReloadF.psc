Scriptname ManualReloadF Native
{Functions of ManualReloadF.dll.}

int Function PollCommand() Native Global
int Function CommandKind() Native Global
int Function CommandRefID() Native Global
int Function CommandToken() Native Global
float[] Function CommandPose() Native Global
float[] Function FreshPose(int aiToken) Native Global
Function AddMag(ObjectReference akMag, int aiToken) Native Global
Function Notify(int aiMessage) Native Global
