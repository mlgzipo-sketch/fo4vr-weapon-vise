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
Static Property ZoneFrame Auto Const            ; corner brackets above the vise
Container Property WV_Stash Auto Const          ; hidden box: other copies of the pistol wait here while this one is equipped
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
int Property CmdFramePose = 9 AutoReadOnly
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
	PlaceFrame()
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
	if !Ready()
		return
	endif
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
	if !Ready()
		return
	endif
	if akDestContainer || !akItemReference
		return
	endif
	if !(akBaseItem as MiscObject)
		return   ; only loose mods turn into parts: never touch a dropped weapon
	endif
	int i = WV_LooseMods.Find(akBaseItem)
	if i < 0 || Parts.Length >= MaxParts
		return
	endif
	WeaponVise.Notify(11)
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

; False for a stale copy of this script left in a save (its quest id now names another record):
; without properties it must do nothing at all.
bool Function Ready()
	return WorkbenchKeyword && WV_Weapons && WV_LooseMods && WV_PartItems && WV_Mods
EndFunction

Event OnTimer(int aiTimerID)
	if !Ready()
		return
	endif
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

ObjectReference FrameRef

; The zone frame waits at the bench; the plugin shows it, places it and hides it.
Function PlaceFrame()
	if !Bench || !ZoneFrame
		return
	endif
	if !FrameRef
		FrameRef = Bench.PlaceAtMe(ZoneFrame, 1, true, false, false)
	else
		FrameRef.MoveTo(Bench)
	endif
	WaitFor3D(FrameRef)
	WeaponVise.SetFrame(FrameRef)
EndFunction

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
		PlaceFrame()
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
			if WeaponVise.CommandFlag() == 2
				ReleaseToHand()
			else
				Unclamp(true)
			endif
		elseif cmd == CmdRemove
			RemovePart(CmdMod())
		elseif cmd == CmdAttach
			AttachPart(CmdPart(), CmdMod())
		elseif cmd == CmdDrop
			DropPart(CmdPart())
		elseif cmd == CmdMove
			MoveWeapon()
		elseif cmd == CmdGhost
			ShowGhost(CmdMod(), WeaponVise.CommandPose()[0] as int)
		elseif cmd == CmdSound
			PlaySound(WeaponVise.CommandPose()[0] as int)
		elseif cmd == CmdFramePose
			if FrameRef
				float[] fp = WeaponVise.CommandPose()
				FrameRef.SetAngle(0.0, 0.0, fp[3])
				FrameRef.SetPosition(fp[0], fp[1], fp[2])
			endif
		endif
		cmd = WeaponVise.PollCommand()
	endwhile
	if Clamped && Clamped.GetBaseObject() as Weapon && WeaponVise.ModelOutdated()
		Reload3D()
	endif
EndFunction

ObjectMod Function CmdMod()
	int id = WeaponVise.CommandModID()
	if id == 0
		return None
	endif
	return Game.GetForm(id) as ObjectMod
EndFunction

ObjectReference Function CmdPart()
	int id = WeaponVise.CommandPartID()
	if id == 0
		return None
	endif
	return Game.GetForm(id) as ObjectReference
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
	float bestDistance = 1600.0 ; 40 units, squared
	int i = 0
	while i < found.Length
		ObjectReference r = found[i]
		if r && !r.GetContainer() && !r.IsDisabled() && r.Is3DLoaded() && r != Clamped
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
	if Clamped && WeaponVise.CommandFlag() == 1
		; the vise held a weapon somewhere else (an old save): it goes back to the inventory
		Unclamp(false)
	endif
	if Clamped
		return
	endif
	Actor player = Game.GetPlayer()
	float[] pose = WeaponVise.CommandPose()
	Weapon w = player.GetEquippedWeapon()
	bool drawn = player.IsWeaponDrawn()
	; a weapon let go here: carried loose, or just dropped by ROCK out of the hand
	ObjectReference ref = None
	int tries = 0
	while !ref && tries < 8
		ref = FindDroppedWeapon(pose)
		if !ref
			Utility.Wait(0.05)
		endif
		tries += 1
	endwhile
	if ref
		w = ref.GetBaseObject() as Weapon
		WeaponVise.Notify(10)
	elseif w && drawn && WV_Weapons.Find(w) >= 0 && player.GetEquippedWeapon() == w
		player.UnequipItem(w, false, true)
		ref = player.DropObject(w, 1)
		if !ref
			WeaponVise.Notify(3)
			player.EquipItem(w, false, true)
			return
		endif
	else
		if w && drawn && WV_Weapons.Find(w) < 0
			WeaponVise.Notify(2)
		endif
		return
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
	ObjectMod filler = Game.GetForm(WeaponVise.FallbackID(m)) as ObjectMod
	if filler
		Clamped.AttachMod(filler)
	endif
	WeaponVise.SetWeapon(Clamped, Clamped.GetAllMods())
	; a normal physical object in the palm: ROCK takes it into the fingers (grip is still held)
	SpawnPart(i, pose, WeaponVise.RemoveIntoHand())
EndFunction

Function AttachPart(ObjectReference part, ObjectMod m)
	int k = Parts.Find(part)
	if !Clamped || !m || k < 0
		return
	endif
	ObjectMod[] before = Clamped.GetAllMods()
	if !Clamped.AttachMod(m)
		WeaponVise.Notify(5)
		; the snap took it out of physics: rebuild it as a normal object
		part.Disable()
		part.Enable()
		WaitFor3D(part)
		part.SetMotionType(1, true)
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
		; a stock filler part (put on when the real one came off) just vanishes:
		; spawning it was the "duplicated barrel"
		if old && old != m && after.Find(old) < 0 && !WeaponVise.IsFreeMod(old)
			int j = WV_Mods.Find(old)
			if j >= 0 && Parts.Length < MaxParts
				SpawnPart(j, WeaponVise.CommandPose(), false)
			elseif old.GetLooseMod()
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
		part.SetMotionType(1, true)   ; let go: it falls and ROCK can pick it up (as in 0.4)
	endif
EndFunction

; Some mod changes do not show until the weapon's 3D is rebuilt.
Function Reload3D()
	WeaponVise.Notify(12)
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
		; made at the weapon: a model far from where its reference is may be culled by the renderer
		ObjectReference at = Clamped
		if !at
			at = Game.GetPlayer()
		endif
		ObjectReference ghost = at.PlaceAtMe(WV_Ghosts.GetAt(i), 1, false, false, false)
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
	if color != 0 && Clamped && g.GetDistance(Clamped) > 30.0
		g.MoveTo(Clamped)
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
		if Ghosts[i] && Ghosts[i].GetBaseObject() as Static
			WeaponVise.Notify(13)
			Ghosts[i].Disable()
			Ghosts[i].Delete()
		endif
		i += 1
	endwhile
	Ghosts = new ObjectReference[0]
	GhostMods = new ObjectMod[0]
	GhostColors = new int[0]
EndFunction

; Grip on the clamped weapon: it gets its physics back where it is, in the gripping hand,
; and ROCK takes it like any loose weapon (as it does with the parts).
Function ReleaseToHand()
	ObjectReference ref = Clamped
	Clamped = None
	WeaponVise.SetWeapon(None, new ObjectMod[0])
	ClearGhosts()
	if ref
		ref.Disable()
		ref.Enable()
		WaitFor3D(ref)
		ref.SetMotionType(1, true)
	endif
EndFunction

Function Unclamp(bool equip)
	Actor player = Game.GetPlayer()
	ObjectReference ref = Clamped
	Clamped = None
	WeaponVise.SetWeapon(None, new ObjectMod[0])
	if ref
		ref.SetMotionType(1, true)
		if equip && ClampedBase
			; EquipItem takes the first pistol of that kind in the backpack, not this one:
			; other copies wait in a hidden box until this one is in the hands
			ObjectReference stash = None
			int others = player.GetItemCount(ClampedBase)
			if others > 0 && WV_Stash
				stash = player.PlaceAtMe(WV_Stash, 1, false, true, false)
				if stash
					player.RemoveItem(ClampedBase, others, true, stash)
				endif
			endif
			player.AddItem(ref, 1, true)
			player.EquipItem(ClampedBase, false, true)
			if stash
				Utility.Wait(0.2)
				stash.RemoveAllItems(player, false)
				stash.Delete()
			endif
			WeaponVise.Notify(6)
		else
			player.AddItem(ref, 1, true)
		endif
	endif
	ClearGhosts()
EndFunction
