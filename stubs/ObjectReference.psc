Scriptname ObjectReference extends Form Native
Function AddItem(Form akItemToAdd, int aiCount = 1, bool abSilent = false) native
bool Function AttachMod(ObjectMod akMod, int aiAttachIndex = 0) native
Function Delete() native
Function Disable(bool abFadeOut = false) native
ObjectReference Function DropObject(Form akObject, int aiCount = 1) native
Function Enable(bool abFadeIn = false) native
ObjectReference[] Function FindAllReferencesWithKeyword(Form akKeywordOrList, float afRadius) native
float Function GetDistance(ObjectReference akOther) native
int Function GetItemCount(Form akItem = None) native
ObjectReference Function GetContainer() native
Cell Function GetParentCell() native
bool Function Is3DLoaded() native
bool Function IsDeleted() native
ObjectReference Function PlaceAtMe(Form akFormToPlace, int aiCount = 1, bool abForcePersist = false, bool abInitiallyDisabled = false, bool abDeleteWhenAble = true) native
Function RemoveItem(Form akItemToRemove, int aiCount = 1, bool abSilent = false, ObjectReference akOtherContainer = None) native
Function RemoveMod(ObjectMod akMod) native
Function SetAngle(float afXAngle, float afYAngle, float afZAngle) native
Function SetMotionType(int aeMotionType, bool abAllowActivate = true) native
Function SetPosition(float afX, float afY, float afZ) native
ObjectMod[] Function GetAllMods() native
Event OnItemAdded(Form akBaseItem, int aiItemCount, ObjectReference akItemReference, ObjectReference akSourceContainer)
EndEvent
