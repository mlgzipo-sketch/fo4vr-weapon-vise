Scriptname RigVR Native
{Functions of RigVR.dll.}

int Function PollCommand() Native Global
int Function CommandKind() Native Global
int Function CommandRefID() Native Global
int Function CommandFormID() Native Global
int Function CommandToken() Native Global
float[] Function CommandPose() Native Global
Function AddRef(ObjectReference akRef, int aiToken) Native Global
Function SetForm(int aiIndex, int aiFormID) Native Global
Function SetMannequin(ObjectReference akRef) Native Global
Function ClearState() Native Global
Function PushState(int aiValue) Native Global
Function StateDone() Native Global
int[] Function GetState() Native Global
Function Notify(int aiMessage) Native Global
