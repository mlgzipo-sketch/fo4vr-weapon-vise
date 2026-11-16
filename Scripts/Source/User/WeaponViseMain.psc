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
Static Property WV_Pegboard Auto Const          ; the crafting stand built in a settlement
Message Property WV_CraftConfirm Auto Const     ; "Create this mod?" Yes / No
Keyword Property WorkshopItemKeyword Auto Const ; bench -> its settlement (the stash)

int Property CmdClamp = 1 AutoReadOnly
int Property CmdUnclamp = 2 AutoReadOnly
int Property CmdRemove = 3 AutoReadOnly
int Property CmdAttach = 4 AutoReadOnly
int Property CmdDrop = 5 AutoReadOnly
int Property CmdMove = 6 AutoReadOnly
int Property CmdGhost = 7 AutoReadOnly
int Property CmdSound = 8 AutoReadOnly
int Property CmdFramePose = 9 AutoReadOnly
int Property CmdBoardHover = 10 AutoReadOnly
int Property CmdBoardTake = 11 AutoReadOnly
int Property CmdBoardPlace = 12 AutoReadOnly
int Property CmdBoardStore = 13 AutoReadOnly
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
ObjectReference[] Boards      ; pegboards near the bench, nearest first
ObjectReference[] BoardRefs   ; the part models shown on them
int[] BoardModIds
int[] BoardStates             ; 0 grey (can not craft), 1 grey (can craft), 2 owned
ObjectReference ReturnBench   ; the bench just left: what is left on it goes to the stash
int BoardTicks

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
	if Clamped && (Clamped.IsDeleted() || Clamped.GetContainer())
		Clamped = None   ; the clamped weapon went into a backpack: the vise is free
	endif
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
		if Bench
			ReturnBench = Bench
		endif
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
			StashClamped()
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
	if ReturnBench && !Busy && player.GetDistance(ReturnBench) > 700.0
		Busy = true
		ReturnToStash(ReturnBench)
		ReturnBench = None
		Busy = false
	endif
	if Clamped && Bench && WV_Pegboard && !Busy
		BoardTicks += 1
		if BoardTicks >= 3
			BoardTicks = 0
			Busy = true
			; a stand built or removed while the weapon is in the vise
			ObjectReference[] near = Bench.FindAllReferencesOfType(WV_Pegboard, 700.0)
			int count = 0
			if near
				count = near.Length
			endif
			int had = 0
			if Boards
				had = Boards.Length
			endif
			if count != had
				RefreshBoard()
			elseif BoardRefs && BoardRefs.Length > 0
				RefreshStates()
			endif
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
			int flag = WeaponVise.CommandFlag()
			if flag == 2
				ReleaseToHand()
			elseif flag == 3
				ForgetClamped()
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
		elseif cmd == CmdBoardHover
			BoardHover(CmdPart(), WeaponVise.CommandFlag())
		elseif cmd == CmdBoardTake
			BoardTake(CmdPart(), WeaponVise.CommandModID())
		elseif cmd == CmdBoardPlace
			ObjectReference bp = CmdPart()
			if bp
				float[] bpose = WeaponVise.CommandPose()
				bp.SetPosition(bpose[0], bpose[1], bpose[2])
			endif
		elseif cmd == CmdBoardStore
			StorePart(CmdPart())
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
	RefreshBoard()
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
	if !Clamped
		ClearBoard()
	endif
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

; The clamped weapon is gone from the world (no 3D for a while): the vise lets it go, and it stays where it is.
Function ForgetClamped()
	Clamped = None
	WeaponVise.SetWeapon(None, new ObjectMod[0])
	ClearGhosts()
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

; ------------------------------------------------------------ the pegboard (crafting stand)

ObjectReference Function WorkshopOf(ObjectReference b)
	if b && WorkshopItemKeyword
		return b.GetLinkedRef(WorkshopItemKeyword)
	endif
	return None
EndFunction

; The weapon left in the vise goes to the settlement's stash (the inventory if there is none).
Function StashClamped()
	ObjectReference ref = Clamped
	ObjectReference b = Bench
	if !b
		b = ReturnBench
	endif
	ObjectReference ws = WorkshopOf(b)
	Clamped = None
	WeaponVise.SetWeapon(None, new ObjectMod[0])
	ClearGhosts()
	if ref
		ref.SetMotionType(1, true)
		if ws
			ws.AddItem(ref, 1, true)
		else
			Game.GetPlayer().AddItem(ref, 1, true)
		endif
	endif
EndFunction

; Parts left lying at a bench the player walked away from go back to its stash as loose mods.
Function ReturnToStash(ObjectReference b)
	ObjectReference ws = WorkshopOf(b)
	Actor player = Game.GetPlayer()
	int i = Parts.Length - 1
	while i >= 0
		ObjectReference p = Parts[i]
		if p && !p.GetContainer() && p.GetDistance(b) < 250.0
			int k = WV_Mods.Find(PartMods[i])
			Parts.Remove(i)
			PartMods.Remove(i)
			WeaponVise.ForgetPart(p)
			p.Disable()
			p.Delete()
			if k >= 0
				if ws
					ws.AddItem(WV_LooseMods.GetAt(k), 1, true)
				else
					player.AddItem(WV_LooseMods.GetAt(k), 1, true)
				endif
			endif
		endif
		i -= 1
	endwhile
EndFunction

int Function CountOf(Form f, ObjectReference ws)
	Actor player = Game.GetPlayer()
	int n = 0
	if f as Component
		n = player.GetComponentCount(f)
		if ws
			n += ws.GetComponentCount(f)
		endif
	else
		n = player.GetItemCount(f)
		if ws
			n += ws.GetItemCount(f)
		endif
	endif
	return n
EndFunction

int Function OwnedCount(int k, ObjectReference ws)
	Form loose = WV_LooseMods.GetAt(k)
	int n = Game.GetPlayer().GetItemCount(loose)
	if ws
		n += ws.GetItemCount(loose)
	endif
	return n
EndFunction

bool Function CanCraft(int modId, ObjectReference ws)
	int[] recipe = WeaponVise.RecipeOf(modId)
	if !recipe || recipe.Length == 0
		return false
	endif
	Actor player = Game.GetPlayer()
	int[] perks = WeaponVise.PerksOf(modId)
	int i = 0
	while perks && i < perks.Length
		Perk p = Game.GetForm(perks[i]) as Perk
		if p && !player.HasPerk(p)
			return false
		endif
		i += 1
	endwhile
	i = 0
	while i + 1 < recipe.Length
		Form f = Game.GetForm(recipe[i])
		if f && CountOf(f, ws) < recipe[i + 1]
			return false
		endif
		i += 2
	endwhile
	return true
EndFunction

int Function StateOf(int modId, ObjectReference ws)
	int k = WV_Mods.Find(Game.GetForm(modId))
	if k >= 0 && OwnedCount(k, ws) > 0
		return 2
	elseif CanCraft(modId, ws)
		return 1
	endif
	return 0
EndFunction

; Shows the clamped weapon's parts on the pegboards near the bench.
Function RefreshBoard()
	if !Clamped || !ClampedBase || !Bench || !WV_Pegboard
		ClearBoard()
		return
	endif
	ObjectReference[] found = Bench.FindAllReferencesOfType(WV_Pegboard, 700.0)
	if !found || found.Length == 0
		ClearBoard()
		return
	endif
	; nearest to the bench first
	int a = 0
	while a < found.Length
		int best = a
		int c = a + 1
		while c < found.Length
			if found[c].GetDistance(Bench) < found[best].GetDistance(Bench)
				best = c
			endif
			c += 1
		endwhile
		ObjectReference t = found[a]
		found[a] = found[best]
		found[best] = t
		a += 1
	endwhile
	if !BoardRefs
		BoardRefs = new ObjectReference[0]
		BoardModIds = new int[0]
		BoardStates = new int[0]
	endif
	ObjectReference ws = WorkshopOf(Bench)
	int[] ids = WeaponVise.BoardMods(ClampedBase.GetFormID())
	ObjectReference[] refs = new ObjectReference[0]
	int[] mods = new int[0]
	int[] states = new int[0]
	int i = 0
	while i < ids.Length && refs.Length < 100
		int k = WV_Mods.Find(Game.GetForm(ids[i]))
		if k >= 0
			ObjectReference r = None
			int old = BoardModIds.Find(ids[i])
			if old >= 0
				r = BoardRefs[old]
			endif
			if !r
				r = Bench.PlaceAtMe(WV_Ghosts.GetAt(k), 1, false, false, false)
			endif
			if r
				refs.Add(r)
				mods.Add(ids[i])
				states.Add(StateOf(ids[i], ws))
			endif
		endif
		i += 1
	endwhile
	; models of another weapon's parts go away
	i = 0
	while i < BoardRefs.Length
		if BoardRefs[i] && refs.Find(BoardRefs[i]) < 0
			BoardRefs[i].Disable()
			BoardRefs[i].Delete()
		endif
		i += 1
	endwhile
	Boards = found
	BoardRefs = refs
	BoardModIds = mods
	BoardStates = states
	WeaponVise.SetBoard(Boards, BoardRefs, BoardModIds, BoardStates)
EndFunction

; Owned or not, can be made or not: the counts change as things are crafted, taken or picked up.
Function RefreshStates()
	if !BoardRefs || BoardRefs.Length == 0
		return
	endif
	ObjectReference ws = WorkshopOf(Bench)
	bool changed = false
	int i = 0
	while i < BoardModIds.Length
		int st = StateOf(BoardModIds[i], ws)
		if st != BoardStates[i]
			BoardStates[i] = st
			changed = true
		endif
		i += 1
	endwhile
	if changed
		WeaponVise.SetBoard(Boards, BoardRefs, BoardModIds, BoardStates)
	endif
EndFunction

Function ClearBoard()
	int i = 0
	while BoardRefs && i < BoardRefs.Length
		if BoardRefs[i]
			BoardRefs[i].Disable()
			BoardRefs[i].Delete()
		endif
		i += 1
	endwhile
	BoardRefs = new ObjectReference[0]
	BoardModIds = new int[0]
	BoardStates = new int[0]
	Boards = new ObjectReference[0]
	WeaponVise.SetBoard(Boards, BoardRefs, BoardModIds, BoardStates)
	WeaponVise.Panel(None, "")
EndFunction

string Function NameOf(Form f)
	if f
		string n = f.GetName()
		if n != ""
			return n
		endif
	endif
	return "?"
EndFunction

; What the hand points at, for the text above it: ~w ~g ~r ~y colours, | new line.
string Function BoardText(int modId, int st, ObjectReference ws)
	int k = WV_Mods.Find(Game.GetForm(modId))
	if k < 0
		return ""
	endif
	string text = "~w" + NameOf(WV_LooseMods.GetAt(k)) + "|"
	if st == 2
		return text + "~gЕсть: " + OwnedCount(k, ws) + ". Грип: взять"
	endif
	int[] recipe = WeaponVise.RecipeOf(modId)
	if !recipe || recipe.Length == 0
		return text + "~rНе создаётся"
	endif
	Actor player = Game.GetPlayer()
	int i = 0
	while i + 1 < recipe.Length
		Form f = Game.GetForm(recipe[i])
		if f
			int have = CountOf(f, ws)
			if have >= recipe[i + 1]
				text += "~g"
			else
				text += "~r"
			endif
			text += NameOf(f) + " " + have + "/" + recipe[i + 1] + "|"
		endif
		i += 2
	endwhile
	int[] perks = WeaponVise.PerksOf(modId)
	i = 0
	while perks && i < perks.Length
		Perk p = Game.GetForm(perks[i]) as Perk
		if p
			if player.HasPerk(p)
				text += "~g"
			else
				text += "~r"
			endif
			text += "Навык: " + NameOf(p) + "|"
		endif
		i += 1
	endwhile
	if st == 1
		return text + "~yГрип: создать"
	endif
	return text + "~rНе хватает"
EndFunction

Function BoardHover(ObjectReference r, int entered)
	int b = -1
	if r && BoardRefs
		b = BoardRefs.Find(r)
	endif
	if b < 0
		return
	endif
	OutlineAttach.Stop(r)
	OutlineRemove.Stop(r)
	if entered == 0
		WeaponVise.Panel(None, "")
		return
	endif
	int st = BoardStates[b]
	if st == 1
		OutlineAttach.Play(r, -1.0)
	elseif st == 0
		OutlineRemove.Play(r, -1.0)
	endif
	WeaponVise.Panel(r, BoardText(BoardModIds[b], st, WorkshopOf(Bench)))
EndFunction

; Grip on a part on the board: an owned one comes into the palm (ROCK takes it);
; one not made yet asks to be crafted from the inventory and the settlement's stash.
Function BoardTake(ObjectReference r, int modId)
	int b = -1
	if r && BoardRefs
		b = BoardRefs.Find(r)
	endif
	if b < 0
		return
	endif
	int k = WV_Mods.Find(Game.GetForm(modId))
	if k < 0
		return
	endif
	Actor player = Game.GetPlayer()
	ObjectReference ws = WorkshopOf(Bench)
	Form loose = WV_LooseMods.GetAt(k)
	int st = StateOf(modId, ws)
	if st == 2
		if player.GetItemCount(loose) > 0
			player.RemoveItem(loose, 1, true)
		elseif ws
			ws.RemoveItem(loose, 1, true)
		endif
		SpawnPart(k, WeaponVise.CommandPose(), WeaponVise.RemoveIntoHand())
		PlaySound(3)
	elseif st == 1
		OutlineAttach.Stop(r)
		if WV_CraftConfirm && WV_CraftConfirm.Show() == 0
			if CanCraft(modId, ws)
				Consume(modId, ws)
				player.AddItem(loose, 1, true)
				PlaySound(2)
				WeaponVise.Hud("Создано: " + NameOf(loose))
			endif
		endif
	endif
	RefreshStates()
	WeaponVise.Panel(r, BoardText(modId, StateOf(modId, ws), ws))
EndFunction

; A part put or thrown onto a stand goes back to the settlement's stash (the inventory if there is none).
Function StorePart(ObjectReference part)
	int k = -1
	if part
		k = Parts.Find(part)
	endif
	if k < 0
		return
	endif
	int i = WV_Mods.Find(PartMods[k])
	Parts.Remove(k)
	PartMods.Remove(k)
	WeaponVise.ForgetPart(part)
	part.Disable()
	part.Delete()
	if i >= 0
		Form loose = WV_LooseMods.GetAt(i)
		ObjectReference ws = WorkshopOf(Bench)
		if ws
			ws.AddItem(loose, 1, true)
		else
			Game.GetPlayer().AddItem(loose, 1, true)
		endif
		WeaponVise.Hud("Убрано на стенд: " + NameOf(loose))
	endif
	PlaySound(2)
	RefreshStates()
EndFunction

; Takes the recipe's components from the inventory first, then from the stash.
Function Consume(int modId, ObjectReference ws)
	int[] recipe = WeaponVise.RecipeOf(modId)
	Actor player = Game.GetPlayer()
	int i = 0
	while i + 1 < recipe.Length
		Form f = Game.GetForm(recipe[i])
		int need = recipe[i + 1]
		if f
			Component c = f as Component
			if c
				int mine = player.GetComponentCount(f)
				if mine > need
					mine = need
				endif
				if mine > 0
					player.RemoveComponents(c, mine, true)
				endif
				if need - mine > 0 && ws
					ws.RemoveComponents(c, need - mine, true)
				endif
			else
				int have = player.GetItemCount(f)
				if have > need
					have = need
				endif
				if have > 0
					player.RemoveItem(f, have, true)
				endif
				if need - have > 0 && ws
					ws.RemoveItem(f, need - have, true)
				endif
			endif
		endif
		i += 2
	endwhile
EndFunction
