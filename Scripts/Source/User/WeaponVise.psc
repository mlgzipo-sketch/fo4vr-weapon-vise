Scriptname WeaponVise Native
{Functions of WeaponVise.dll (F4SE plugin) for the workbench script.}

Function SetWorkbench(ObjectReference akBench) global native
Function SetWeapon(ObjectReference akWeapon, ObjectMod[] akMods) global native
Function AddPart(ObjectReference akPart, ObjectMod akMod, bool abInHand) global native
Function ForgetPart(ObjectReference akPart) global native
; An outline model of this mod's part, placed and shown by the plugin.
Function SetFrame(ObjectReference akFrame) global native
Function AddGhost(ObjectReference akGhost, ObjectMod akMod) global native
int Function PollCommand() global native
; Form ids, not forms: a form returned by a plugin function crashed the VM (CreateObject)
int Function CommandModID() global native
int Function CommandFlag() global native
int Function CommandPartID() global native
float[] Function CommandPose() global native
float[] Function RackPose(int aiIndex) global native
bool Function ModelOutdated() global native
Function Notify(int aiMessage) global native
; The mod that fills a slot when this part is taken off (iron sights, stock barrel), or None.
int Function FallbackID(ObjectMod akMod) global native
; True for such fillers: they come for free and are not given back as loose mods.
bool Function IsFreeMod(ObjectMod akMod) global native
; True: a removed part is carried by the plugin (old way); false: it is a physical object ROCK takes.
bool Function RemoveIntoHand() global native
