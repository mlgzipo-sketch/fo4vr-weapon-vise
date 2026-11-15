Scriptname RigVRMain extends Quest
{RigVR, the game side. RigVR.dll decides everything and draws the rig; this script makes and removes the
objects it draws with, moves items between the inventory and the hands, and keeps the rig in the save.}

FormList Property RigVR_Stats Auto Const        ; 0 mannequin, 1 rig, 2 free place, 3 cursor, 4.. pouches (type 1..6)
FormList Property RigVR_PouchItems Auto Const   ; pouch items kind 1..6, then the rig item (kind 7)
FormList Property RigVR_OldItems Auto Const     ; pouch items of 0.1-0.3, taken out of the inventory
FormList Property RigVR_Sounds Auto Const       ; 0 snap, 1 pick up, 2 focus, 3 put in
MiscObject Property Kit Auto Const              ; the folded mannequin

int Property TimerPoll = 1 AutoReadOnly
int Property MaxSpawned = 128 AutoReadOnly

int[] Saved                 ; the rig as RigVR.GetState() gave it
ObjectReference[] Spawned   ; objects made for drawing (ring buffer), removed after a load
int SpawnedNext
bool Busy
bool KitGiven
int KitsToIgnore             ; kits this script removes itself: their 'dropped' event sets nothing up
bool SlingGiven             ; 0.2: the back sling came after the first starter kit

Event OnInit()
	RegisterForRemoteEvent(Game.GetPlayer(), "OnPlayerLoadGame")
	Spawned = new ObjectReference[128]
	SendForms()
	SendState()
	if !KitGiven
		KitGiven = true
		SlingGiven = true
		GiveStarterKit()
	endif
	GiveNewItems()
	StartTimer(0.1, TimerPoll)
EndEvent

Event Actor.OnPlayerLoadGame(Actor akSender)
	Busy = false
	Cleanup()
	SendForms()
	SendState()
	GiveNewItems()
	StartTimer(0.1, TimerPoll)
EndEvent

; 0.4: the pouches of 0.1-0.3 are gone (the rig now starts on the mannequin with the new ones); old items leave the inventory.
Function GiveNewItems()
	SlingGiven = true
	if RigVR_OldItems
		Actor player = Game.GetPlayer()
		int i = 0
		while i < RigVR_OldItems.GetSize()
			Form f = RigVR_OldItems.GetAt(i)
			int n = player.GetItemCount(f)
			if n > 0
				player.RemoveItem(f, n, true)
			endif
			i += 1
		endwhile
	endif
	; 0.4 test build set up a mannequin for every old item it took away (each one folded the last into a kit): one kit is enough
	int kits = Game.GetPlayer().GetItemCount(Kit)
	if kits > 1
		KitsToIgnore += kits - 1
		Game.GetPlayer().RemoveItem(Kit, kits - 1, true)
	endif
EndFunction

Function SendForms()
	RigVR.SetForm(0, Kit.GetFormID())
	int i = 0
	while i < RigVR_PouchItems.GetSize()
		RigVR.SetForm(i + 1, RigVR_PouchItems.GetAt(i).GetFormID())
		i += 1
	endwhile
EndFunction

Function SendState()
	RigVR.ClearState()
	if Saved
		int i = 0
		while i < Saved.Length
			RigVR.PushState(Saved[i])
			i += 1
		endwhile
	endif
	RigVR.StateDone()
EndFunction

Function GiveStarterKit()
	Actor player = Game.GetPlayer()
	; the rig with its pouches starts on the mannequin (RigVR.dll keeps it), so only the mannequin itself is given
	player.AddItem(Kit, 1, true)
	RigVR.Notify(20)
EndFunction

Function Cleanup()
	if !Spawned
		Spawned = new ObjectReference[128]
		return
	endif
	int i = 0
	while i < Spawned.Length
		ObjectReference r = Spawned[i]
		if r
			r.Disable()
			r.Delete()
			Spawned[i] = None
		endif
		i += 1
	endwhile
	SpawnedNext = 0
EndFunction

Function Remember(ObjectReference r)
	if !Spawned
		Spawned = new ObjectReference[128]
	endif
	Spawned[SpawnedNext] = r
	SpawnedNext += 1
	if SpawnedNext >= MaxSpawned
		SpawnedNext = 0
	endif
EndFunction

Event OnTimer(int aiTimerID)
	if !Busy
		Busy = true
		Handle()
		Busy = false
	endif
	StartTimer(0.05, TimerPoll)
EndEvent

Function Handle()
	Actor player = Game.GetPlayer()
	int cmd = RigVR.PollCommand()
	int n = 0
	while cmd != 0 && n < 16
		ObjectReference r = None
		Form f = None
		if cmd == 2 || cmd == 3 || cmd == 4 || cmd == 7 || cmd == 9 || cmd == 14
			r = Game.GetForm(RigVR.CommandRefID()) as ObjectReference
		endif
		if cmd == 5 || cmd == 6 || cmd == 8 || cmd == 10 || cmd == 11
			f = Game.GetForm(RigVR.CommandFormID())
		endif
		if cmd == 1
			Spawn(RigVR.CommandKind(), RigVR.CommandToken())
		elseif cmd == 2
			if r
				r.Disable()
				r.Delete()
			endif
		elseif cmd == 3
			if r
				float[] p = RigVR.CommandPose()
				r.SetPosition(p[0], p[1], p[2])
			endif
		elseif cmd == 4
			if r
				r.MoveTo(player)
			endif
		elseif cmd == 5
			if f
				player.RemoveItem(f, 1, true)
			endif
		elseif cmd == 6
			if f
				player.AddItem(f, 1, true)
			endif
		elseif cmd == 7
			if r
				player.AddItem(r, 1, true)
			endif
		elseif cmd == 8
			if f
				DropToHand(f, RigVR.CommandPose(), RigVR.CommandToken())
			endif
		elseif cmd == 9
			if r
				Form base = r.GetBaseObject()
				player.AddItem(r, 1, true)
				player.EquipItem(base, false, true)
			endif
		elseif cmd == 10
			if f
				player.EquipItem(f, false, true)
			endif
		elseif cmd == 11
			if f
				player.UnequipItem(f, false, true)
				Recollect(f)
			endif
		elseif cmd == 12
			PlaySound(RigVR.CommandKind())
		elseif cmd == 13
			Saved = RigVR.GetState()
		elseif cmd == 14
			if r
				r.Disable()
				r.Delete()
			endif
			player.AddItem(Kit, 1, true)
		endif
		n += 1
		cmd = RigVR.PollCommand()
	endwhile
EndFunction

; A gun put in the holster / on the back: if ROCK dropped it as the grip was let go, it comes back.
Function Recollect(Form f)
	Actor player = Game.GetPlayer()
	int tries = 0
	while tries < 4
		ObjectReference[] near = player.FindAllReferencesOfType(f, 150.0)
		int i = 0
		while near && i < near.Length
			if near[i] && !near[i].IsDisabled()
				player.AddItem(near[i], 1, true)
			endif
			i += 1
		endwhile
		Utility.Wait(0.15)
		tries += 1
	endwhile
EndFunction

Function Spawn(int kind, int token)
	if kind < 0 || kind >= RigVR_Stats.GetSize()
		return
	endif
	ObjectReference r = Game.GetPlayer().PlaceAtMe(RigVR_Stats.GetAt(kind), 1, false, false, true)
	if r
		Remember(r)
		RigVR.AddRef(r, token)
	else
		RigVR.Notify(1)
	endif
EndFunction

; Out of the inventory into the hand: ROCK catches it while the grip is still held.
Function DropToHand(Form f, float[] pose, int token)
	ObjectReference r = Game.GetPlayer().DropObject(f, 1)
	if !r
		RigVR.Notify(2)
		return
	endif
	if pose && pose.Length >= 3
		r.SetPosition(pose[0], pose[1], pose[2])
	endif
	RigVR.AddRef(r, token)
EndFunction

; The kit dropped from the Pip-Boy: the mannequin stands up in front of you, facing you.
; Pouches and the rig are ordinary items: dropped, they just lie there (take one in the hand to hang it on the mannequin).
Function ItemDropped(ObjectReference item, int kind)
	Actor player = Game.GetPlayer()
	if kind != 0
		return
	endif
	if KitsToIgnore > 0
		KitsToIgnore -= 1
		return
	endif
	float a = player.GetAngleZ()
	float x = player.GetPositionX() + Math.Sin(a) * 80.0
	float y = player.GetPositionY() + Math.Cos(a) * 80.0
	float z = player.GetPositionZ()
	ObjectReference m = player.PlaceAtMe(RigVR_Stats.GetAt(0), 1, true, true, false)
	if !m
		player.AddItem(item, 1, true)
		RigVR.Notify(3)
		return
	endif
	m.SetPosition(x, y, z)
	m.SetAngle(0.0, 0.0, a + 180.0)
	m.Enable()
	item.Disable()
	item.Delete()
	RigVR.SetMannequin(m)
EndFunction

Function PlaySound(int index)
	if index >= 0 && index < RigVR_Sounds.GetSize()
		Sound s = RigVR_Sounds.GetAt(index) as Sound
		if s
			s.Play(Game.GetPlayer())
		endif
	endif
EndFunction
