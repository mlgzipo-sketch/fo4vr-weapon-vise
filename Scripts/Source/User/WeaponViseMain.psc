Scriptname WeaponViseMain extends Quest
{Weapon vise: the inventory side. WeaponVise.dll watches the hands and sends
commands (clamp, unclamp, remove a part, attach a part, drop a part); this
script carries them out on the game's records.}

Keyword Property WorkbenchKeyword Auto Const
FormList Property WV_Mods Auto Const          ; mods that are physical parts
FormList Property WV_PartItems Auto Const     ; the part item of each mod (same order)
FormList Property WV_Weapons Auto Const       ; weapons that can be clamped

int Property CmdClamp = 1 AutoReadOnly
int Property CmdUnclamp = 2 AutoReadOnly
int Property CmdRemove = 3 AutoReadOnly
int Property CmdAttach = 4 AutoReadOnly
int Property CmdDrop = 5 AutoReadOnly
int Property TimerScan = 1 AutoReadOnly
int Property TimerFast = 2 AutoReadOnly
int Property MaxParts = 12 AutoReadOnly

ObjectReference Bench
ObjectReference Clamped
Weapon ClampedBase
ObjectReference[] Parts
ObjectMod[] PartMods
bool FastRunning
bool Busy

Event OnInit()
	Setup()
EndEvent

Function Setup()
	if !Parts
		Parts = new ObjectReference[0]
		PartMods = new ObjectMod[0]
	endif
	RegisterForRemoteEvent(Game.GetPlayer(), "OnPlayerLoadGame")
	FastRunning = false
	Busy = false
	StartTimer(1.0, TimerScan)
EndFunction

; The plugin forgets everything when a save is loaded: tell it again.
Event Actor.OnPlayerLoadGame(Actor akSender)
	Setup()
	WeaponVise.SetWorkbench(Bench)
	if Clamped
		WeaponVise.SetWeapon(Clamped, Clamped.GetAllMods())
	endif
	int i = 0
	while i < Parts.Length
		WeaponVise.AddPart(Parts[i], PartMods[i], false)
		i += 1
	endwhile
EndEvent

Event OnTimer(int aiTimerID)
	if aiTimerID == TimerScan
		Scan()
		StartTimer(1.0, TimerScan)
	elseif aiTimerID == TimerFast
		if !Busy
			Busy = true
			Handle()
			Busy = false
		endif
		if Bench || Clamped
			StartTimer(0.1, TimerFast)
		else
			FastRunning = false
		endif
	endif
EndEvent

Function Scan()
	Actor player = Game.GetPlayer()
	ObjectReference nearest = None
	float best = 100000.0
	ObjectReference[] benches = player.FindAllReferencesWithKeyword(WorkbenchKeyword, 400.0)
	int i = 0
	while i < benches.Length
		float d = player.GetDistance(benches[i])
		if d < best
			best = d
			nearest = benches[i]
		endif
		i += 1
	endwhile
	if nearest != Bench
		Bench = nearest
		WeaponVise.SetWorkbench(Bench)
	endif
	if Clamped && !Busy
		if Clamped.GetContainer() == player
			; picked up the normal way (activated it)
			Busy = true
			Clamped = None
			WeaponVise.SetWeapon(None, new ObjectMod[0])
			ReturnParts()
			Busy = false
		elseif !Clamped.Is3DLoaded() || player.GetDistance(Clamped) > 700.0
			Busy = true
			Unclamp(false)
			WeaponVise.Notify(7)
			Busy = false
		endif
	endif
	if (Bench || Clamped) && !FastRunning
		FastRunning = true
		StartTimer(0.1, TimerFast)
	endif
EndFunction

Function Handle()
	int cmd = WeaponVise.PollCommand()
	while cmd != 0
		if cmd == CmdClamp
			Clamp()
		elseif cmd == CmdUnclamp
			Unclamp(true)
		elseif cmd == CmdRemove
			RemovePart(WeaponVise.CommandMod())
		elseif cmd == CmdAttach
			AttachPart(WeaponVise.CommandPart(), WeaponVise.CommandMod())
		elseif cmd == CmdDrop
			DropPart(WeaponVise.CommandPart())
		endif
		cmd = WeaponVise.PollCommand()
	endwhile
	if Clamped && WeaponVise.ModelOutdated()
		Reload3D()
	endif
EndFunction

bool Function WaitFor3D(ObjectReference ref)
	int n = 0
	while ref && !ref.Is3DLoaded() && n < 40
		Utility.Wait(0.05)
		n += 1
	endwhile
	return ref && ref.Is3DLoaded()
EndFunction

Function Clamp()
	if Clamped
		return
	endif
	Actor player = Game.GetPlayer()
	Weapon w = player.GetEquippedWeapon()
	if !w
		WeaponVise.Notify(1)
		return
	endif
	if WV_Weapons.Find(w) < 0
		WeaponVise.Notify(2)
		return
	endif
	float[] pose = WeaponVise.CommandPose()
	player.UnequipItem(w, false, true)
	ObjectReference ref = player.DropObject(w, 1)
	if !ref
		WeaponVise.Notify(3)
		player.EquipItem(w, false, true)
		return
	endif
	WaitFor3D(ref)
	ref.SetMotionType(2, true)
	ref.SetAngle(0.0, 0.0, pose[3])
	ref.SetPosition(pose[0], pose[1], pose[2])
	ref.SetMotionType(2, true)
	Clamped = ref
	ClampedBase = w
	WeaponVise.SetWeapon(ref, ref.GetAllMods())
	LayOutParts()
	WeaponVise.Notify(4)
EndFunction

; Loose mods for this weapon come out of the inventory as real parts.
Function LayOutParts()
	Actor player = Game.GetPlayer()
	int i = 0
	while i < WV_Mods.GetSize() && Parts.Length < MaxParts
		ObjectMod m = WV_Mods.GetAt(i) as ObjectMod
		MiscObject loose = m.GetLooseMod()
		int n = 0
		if loose
			n = player.GetItemCount(loose)
		endif
		while n > 0 && Parts.Length < MaxParts
			player.RemoveItem(loose, 1, true)
			SpawnPart(i, WeaponVise.RackPose(Parts.Length), false)
			n -= 1
		endwhile
		i += 1
	endwhile
EndFunction

ObjectReference Function SpawnPart(int index, float[] pose, bool inHand)
	ObjectMod m = WV_Mods.GetAt(index) as ObjectMod
	ObjectReference part = Game.GetPlayer().PlaceAtMe(WV_PartItems.GetAt(index), 1, true, true, false)
	if !part
		return None
	endif
	part.SetPosition(pose[0], pose[1], pose[2])
	part.SetAngle(0.0, 0.0, pose[3])
	part.Enable()
	Parts.Add(part)
	PartMods.Add(m)
	WeaponVise.AddPart(part, m, inHand)
	return part
EndFunction

Function RemovePart(ObjectMod m)
	if !Clamped || !m
		return
	endif
	int i = WV_Mods.Find(m)
	if i < 0
		return
	endif
	float[] pose = WeaponVise.CommandPose()
	Clamped.RemoveMod(m)
	ObjectMod filler = WeaponVise.FallbackFor(m)
	if filler
		Clamped.AttachMod(filler)
	endif
	WeaponVise.SetWeapon(Clamped, Clamped.GetAllMods())
	SpawnPart(i, pose, true)
EndFunction

Function AttachPart(ObjectReference part, ObjectMod m)
	int k = Parts.Find(part)
	if !Clamped || !m || k < 0
		return
	endif
	ObjectMod[] before = Clamped.GetAllMods()
	if !Clamped.AttachMod(m)
		WeaponVise.Notify(5)
		WeaponVise.AddPart(part, m, false)
		return
	endif
	Parts.Remove(k)
	PartMods.Remove(k)
	WeaponVise.ForgetPart(part)
	part.Delete()
	ObjectMod[] after = Clamped.GetAllMods()
	WeaponVise.SetWeapon(Clamped, after)
	; whatever the new part pushed out goes onto the bench (or back to the inventory)
	int b = 0
	while b < before.Length
		ObjectMod old = before[b]
		if old && old != m && after.Find(old) < 0
			int j = WV_Mods.Find(old)
			if j >= 0 && Parts.Length < MaxParts
				SpawnPart(j, WeaponVise.RackPose(Parts.Length), false)
			elseif !WeaponVise.IsFreeMod(old) && old.GetLooseMod()
				Game.GetPlayer().AddItem(old.GetLooseMod(), 1, true)
			endif
		endif
		b += 1
	endwhile
EndFunction

Function DropPart(ObjectReference part)
	if part && Parts.Find(part) >= 0
		float[] pose = WeaponVise.CommandPose()
		part.SetPosition(pose[0], pose[1], pose[2])
	endif
EndFunction

; Some mod changes do not show until the weapon's 3D is rebuilt.
Function Reload3D()
	Clamped.Disable()
	Clamped.Enable()
	WaitFor3D(Clamped)
	Clamped.SetMotionType(2, true)
	WeaponVise.SetWeapon(Clamped, Clamped.GetAllMods())
EndFunction

Function Unclamp(bool equip)
	Actor player = Game.GetPlayer()
	ObjectReference ref = Clamped
	Clamped = None
	WeaponVise.SetWeapon(None, new ObjectMod[0])
	if ref
		ref.SetMotionType(1, true)
		player.AddItem(ref, 1, true)
		if equip && ClampedBase
			player.EquipItem(ClampedBase, false, true)
			WeaponVise.Notify(6)
		endif
	endif
	ReturnParts()
EndFunction

Function ReturnParts()
	Actor player = Game.GetPlayer()
	int i = Parts.Length - 1
	while i >= 0
		ObjectReference part = Parts[i]
		ObjectMod m = PartMods[i]
		if m && m.GetLooseMod()
			player.AddItem(m.GetLooseMod(), 1, true)
		endif
		WeaponVise.ForgetPart(part)
		if part
			part.Delete()
		endif
		i -= 1
	endwhile
	Parts = new ObjectReference[0]
	PartMods = new ObjectMod[0]
EndFunction
