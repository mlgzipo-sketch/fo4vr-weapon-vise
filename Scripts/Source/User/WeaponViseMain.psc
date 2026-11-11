Scriptname WeaponViseMain extends Quest
{Weapon vise: the inventory side. WeaponVise.dll watches the hands and sends
commands (clamp, unclamp, remove a part, attach a part, drop a part); this
script carries them out on the game's records.}

Keyword Property WorkbenchKeyword Auto Const
FormList Property WV_Mods Auto Const          ; mods that are physical parts
FormList Property WV_PartItems Auto Const     ; the part item of each mod (same order)
FormList Property WV_Weapons Auto Const       ; weapons that can be clamped
FormList Property WV_LooseMods Auto Const     ; the game's loose mod item of each mod (same order)
FormList Property WV_Ghosts Auto Const        ; outline model of each mod's part (same order)
FormList Property WV_Sounds Auto Const        ; focus, focus red, snap, pick up
EffectShader Property OutlineAttach Auto Const
EffectShader Property OutlineRemove Auto Const

int Property CmdClamp = 1 AutoReadOnly
int Property CmdUnclamp = 2 AutoReadOnly
int Property CmdRemove = 3 AutoReadOnly
int Property CmdAttach = 4 AutoReadOnly
int Property CmdDrop = 5 AutoReadOnly
int Property CmdMove = 6 AutoReadOnly
int Property CmdGhost = 7 AutoReadOnly
int Property CmdSound = 8 AutoReadOnly
int Property TimerScan = 1 AutoReadOnly
int Property TimerFast = 2 AutoReadOnly
int Property MaxParts = 120 AutoReadOnly

ObjectReference Bench
ObjectReference Clamped
Weapon ClampedBase
ObjectReference[] Parts
ObjectMod[] PartMods
bool FastRunning
bool Busy
int SkipPartPickups   ; part items we put into the inventory ourselves (attaching)
ObjectReference[] Ghosts
ObjectMod[] GhostMods
int[] GhostColors      ; 0 none, 1 remove (red), 2 attach (green)

Event OnInit()
	Setup()
EndEvent

Function Setup()
	if !Parts
		Parts = new ObjectReference[0]
		PartMods = new ObjectMod[0]
	endif
	Actor player = Game.GetPlayer()
	RegisterForRemoteEvent(player, "OnPlayerLoadGame")
	; Parts and loose mods change into each other when they enter or leave the inventory.
	AddInventoryEventFilter(WV_PartItems)
	AddInventoryEventFilter(WV_LooseMods)
	RegisterForRemoteEvent(player, "OnItemAdded")
	RegisterForRemoteEvent(player, "OnItemRemoved")
	SkipPartPickups = 0
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
	ClearGhosts()
EndEvent

; A part picked up (by hand or by activating it) becomes the game's loose mod,
; so the workbench menu and other weapons can use it.
Event ObjectReference.OnItemAdded(ObjectReference akSender, Form akBaseItem, int aiItemCount, ObjectReference akItemReference, ObjectReference akSourceContainer)
	int i = WV_PartItems.Find(akBaseItem)
	if i < 0
		return
	endif
	if akItemReference
		ForgetPartRef(akItemReference)
	endif
	if SkipPartPickups > 0
		SkipPartPickups -= 1
		return
	endif
	Actor player = Game.GetPlayer()
	player.RemoveItem(akBaseItem, aiItemCount, true)
	player.AddItem(WV_LooseMods.GetAt(i), aiItemCount, true)
EndEvent

; A loose mod dropped into the world turns into the part itself.
Event ObjectReference.OnItemRemoved(ObjectReference akSender, Form akBaseItem, int aiItemCount, ObjectReference akItemReference, ObjectReference akDestContainer)
	if akDestContainer || !akItemReference
		return
	endif
	int i = WV_LooseMods.Find(akBaseItem)
	if i < 0 || Parts.Length >= MaxParts
		return
	endif
	WaitFor3D(akItemReference)
	float[] pose = new float[4]
	pose[0] = akItemReference.GetPositionX()
	pose[1] = akItemReference.GetPositionY()
	pose[2] = akItemReference.GetPositionZ() + 3.0
	pose[3] = 0.0
	akItemReference.Disable()
	akItemReference.Delete()
	int n = aiItemCount
	while n > 0 && Parts.Length < MaxParts
		SpawnPart(i, pose, false)
		pose[2] = pose[2] + 4.0
		n -= 1
	endwhile
EndEvent

Function ForgetPartRef(ObjectReference part)
	int k = Parts.Find(part)
	if k >= 0
		Parts.Remove(k)
		PartMods.Remove(k)
	endif
	WeaponVise.ForgetPart(part)
EndFunction

; Parts that are gone (picked up, deleted) are dropped from the list.
Function PruneParts()
	int i = Parts.Length - 1
	while i >= 0
		ObjectReference part = Parts[i]
		if !part || part.IsDeleted() || part.GetContainer()
			Parts.Remove(i)
			PartMods.Remove(i)
			WeaponVise.ForgetPart(part)
		endif
		i -= 1
	endwhile
EndFunction

Event OnTimer(int aiTimerID)
	if aiTimerID == TimerScan
		Scan()
		if !Busy
			PruneParts()
		endif
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

int Unloaded = 0

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
			WeaponVise.Notify(8)
			Busy = true
			Clamped = None
			WeaponVise.SetWeapon(None, new ObjectMod[0])
			ClearGhosts()
			Busy = false
		elseif player.GetDistance(Clamped) > 700.0
			Busy = true
			Unclamp(false)
			WeaponVise.Notify(7)
			Busy = false
		elseif !Clamped.Is3DLoaded()
			; the model can be away for a moment after a move: give it a few seconds
			Unloaded += 1
			WeaponVise.Notify(9)
			if Unloaded >= 5
				Busy = true
				Unclamp(false)
				WeaponVise.Notify(7)
				Busy = false
			endif
		else
			Unloaded = 0
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
		elseif cmd == CmdMove
			MoveWeapon()
		elseif cmd == CmdGhost
			ShowGhost(WeaponVise.CommandMod(), WeaponVise.CommandPose()[0] as int)
		elseif cmd == CmdSound
			PlaySound(WeaponVise.CommandPose()[0] as int)
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

; A weapon of ours lying loose near the clamp point (ROCK drops the weapon when grip is released).
ObjectReference Function FindDroppedWeapon(float[] pose)
	ObjectReference[] found = Game.GetPlayer().FindAllReferencesOfType(WV_Weapons, 300.0)
	ObjectReference best = None
	float bestDistance = 3600.0 ; 60 units, squared
	int i = 0
	while i < found.Length
		ObjectReference r = found[i]
		if r && !r.GetContainer() && !r.IsDisabled() && r != Clamped
			float dx = r.GetPositionX() - pose[0]
			float dy = r.GetPositionY() - pose[1]
			float dz = r.GetPositionZ() - pose[2]
			float d = dx * dx + dy * dy + dz * dz
			if d < bestDistance
				bestDistance = d
				best = r
			endif
		endif
		i += 1
	endwhile
	return best
EndFunction

Function Clamp()
	if Clamped
		return
	endif
	Actor player = Game.GetPlayer()
	float[] pose = WeaponVise.CommandPose()
	Weapon w = player.GetEquippedWeapon()
	ObjectReference ref = None
	if !w || WV_Weapons.Find(w) < 0
		; ROCK may have just dropped it: give the drop a moment, then take it from the world
		int tries = 0
		while !ref && tries < 10
			Utility.Wait(0.05)
			ref = FindDroppedWeapon(pose)
			tries += 1
		endwhile
		if !ref
			if !w
				WeaponVise.Notify(1)
			else
				WeaponVise.Notify(2)
			endif
			return
		endif
		w = ref.GetBaseObject() as Weapon
		WeaponVise.Notify(10)
	else
		player.UnequipItem(w, false, true)
		ref = player.DropObject(w, 1)
		if !ref
			WeaponVise.Notify(3)
			player.EquipItem(w, false, true)
			return
		endif
	endif
	WaitFor3D(ref)
	ref.SetMotionType(2, true)
	ref.SetAngle(0.0, 0.0, pose[3])
	ref.SetPosition(pose[0], pose[1], pose[2])
	ref.SetMotionType(2, true)
	Unloaded = 0
	Clamped = ref
	ClampedBase = w
	WeaponVise.SetWeapon(ref, ref.GetAllMods())
	WeaponVise.Notify(4)
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
	WaitFor3D(part)
	if inHand
		part.SetMotionType(2, true)   ; keyframed: follows the hand that pulled it off
	else
		part.SetMotionType(1, true)   ; a normal physical object
	endif
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
	; taken like a pickup (what ROCK expects of an object near the hand), then removed
	SkipPartPickups += 1
	Actor player = Game.GetPlayer()
	player.AddItem(part, 1, true)
	player.RemoveItem(WV_PartItems.GetAt(WV_Mods.Find(m)), 1, true)
	ObjectMod[] after = Clamped.GetAllMods()
	WeaponVise.SetWeapon(Clamped, after)
	; whatever the new part pushed out goes onto the bench (or back to the inventory)
	int b = 0
	while b < before.Length
		ObjectMod old = before[b]
		if old && old != m && after.Find(old) < 0
			int j = WV_Mods.Find(old)
			if j >= 0 && Parts.Length < MaxParts
				SpawnPart(j, WeaponVise.CommandPose(), false)
			elseif !WeaponVise.IsFreeMod(old) && old.GetLooseMod()
				player.AddItem(old.GetLooseMod(), 1, true)
			endif
		endif
		b += 1
	endwhile
EndFunction

Function DropPart(ObjectReference part)
	if part && Parts.Find(part) >= 0
		float[] pose = WeaponVise.CommandPose()
		part.SetPosition(pose[0], pose[1], pose[2])
		part.SetMotionType(1, true)   ; let go: it falls and ROCK can pick it up
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

; The weapon was moved by hand inside the vise area: keep the reference there too.
Function MoveWeapon()
	if Clamped
		float[] pose = WeaponVise.CommandPose()
		Clamped.SetAngle(0.0, 0.0, pose[3])
		Clamped.SetPosition(pose[0], pose[1], pose[2])
	endif
EndFunction

Function PlaySound(int index)
	if index >= 0 && index < WV_Sounds.GetSize()
		Sound s = WV_Sounds.GetAt(index) as Sound
		if s
			s.Play(Game.GetPlayer())
		endif
	endif
EndFunction

; Outlines: one ghost model per mod, made on first use; the plugin places and hides it.
Function ShowGhost(ObjectMod m, int color)
	if !Ghosts
		Ghosts = new ObjectReference[0]
		GhostMods = new ObjectMod[0]
		GhostColors = new int[0]
	endif
	int k = GhostMods.Find(m)
	if k < 0
		if color == 0
			return
		endif
		int i = WV_Mods.Find(m)
		if i < 0
			return
		endif
		ObjectReference ghost = Game.GetPlayer().PlaceAtMe(WV_Ghosts.GetAt(i), 1, false, false, false)
		if !ghost
			return
		endif
		WaitFor3D(ghost)
		Ghosts.Add(ghost)
		GhostMods.Add(m)
		GhostColors.Add(0)
		k = Ghosts.Length - 1
		WeaponVise.AddGhost(ghost, m)
	endif
	ObjectReference g = Ghosts[k]
	if GhostColors[k] == color
		return
	endif
	if GhostColors[k] == 1
		OutlineRemove.Stop(g)
	elseif GhostColors[k] == 2
		OutlineAttach.Stop(g)
	endif
	if color == 1
		OutlineRemove.Play(g, -1.0)
	elseif color == 2
		OutlineAttach.Play(g, -1.0)
	endif
	GhostColors[k] = color
EndFunction

Function ClearGhosts()
	int i = 0
	while Ghosts && i < Ghosts.Length
		if Ghosts[i]
			Ghosts[i].Disable()
			Ghosts[i].Delete()
		endif
		i += 1
	endwhile
	Ghosts = new ObjectReference[0]
	GhostMods = new ObjectMod[0]
	GhostColors = new int[0]
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
	ClearGhosts()
EndFunction
