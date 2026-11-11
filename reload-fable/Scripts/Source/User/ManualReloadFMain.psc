Scriptname ManualReloadFMain extends Quest
{Manual reload (the F build): the game side. ManualReloadF.dll draws the magazine near the gun by itself;
this script only turns a dropped magazine into a real object, removes old ones and plays sounds.}

FormList Property MRF_Mags Auto Const      ; magazine items by kind: 0 standard, 1 large
FormList Property MRF_Sounds Auto Const    ; 0 magazine in, 1 magazine out, 2 slide back, 3 slide home, 4 put away

int Property CmdSpawn = 1 AutoReadOnly
int Property CmdDelete = 2 AutoReadOnly
int Property CmdSound = 3 AutoReadOnly
int Property TimerPoll = 1 AutoReadOnly

bool Busy
int Ticks

Event OnInit()
	RegisterForRemoteEvent(Game.GetPlayer(), "OnPlayerLoadGame")
	StartTimer(0.1, TimerPoll)
EndEvent

Event Actor.OnPlayerLoadGame(Actor akSender)
	Busy = false
	StartTimer(0.1, TimerPoll)
EndEvent

Event OnTimer(int aiTimerID)
	if MRF_Mags && MRF_Sounds && !Busy
		Busy = true
		Handle()
		Ticks += 1
		if Ticks >= 40
			Ticks = 0
			Tidy()
		endif
		Busy = false
	endif
	StartTimer(0.03, TimerPoll)
EndEvent

Function Handle()
	int cmd = ManualReloadF.PollCommand()
	int n = 0
	while cmd != 0 && n < 8
		if cmd == CmdSpawn
			Spawn(ManualReloadF.CommandKind(), ManualReloadF.CommandPose(), ManualReloadF.CommandToken())
		elseif cmd == CmdDelete
			ObjectReference ref = Game.GetForm(ManualReloadF.CommandRefID()) as ObjectReference
			if ref
				ref.Disable()
				ref.Delete()
			endif
		elseif cmd == CmdSound
			PlaySound(ManualReloadF.CommandKind())
		endif
		n += 1
		cmd = ManualReloadF.PollCommand()
	endwhile
EndFunction

Function Spawn(int kind, float[] pose, int token)
	if kind < 0 || kind >= MRF_Mags.GetSize()
		kind = 0
	endif
	ObjectReference mag = Game.GetPlayer().PlaceAtMe(MRF_Mags.GetAt(kind), 1, true, true, true)
	if !mag
		ManualReloadF.Notify(1)
		return
	endif
	; the plugin keeps drawing the magazine as it falls: the object appears where the drawn one is now
	float[] fresh = ManualReloadF.FreshPose(token)
	if fresh && fresh.Length == 6
		pose = fresh
	endif
	mag.SetPosition(pose[0], pose[1], pose[2])
	mag.SetAngle(pose[3], pose[4], pose[5])
	mag.Enable()
	int n = 0
	while !mag.Is3DLoaded() && n < 40
		Utility.Wait(0.02)
		n += 1
	endwhile
	fresh = ManualReloadF.FreshPose(token)
	if fresh && fresh.Length == 6
		mag.SetPosition(fresh[0], fresh[1], fresh[2])
	endif
	mag.SetMotionType(1, true)   ; a normal physical object: it lies on the ground, ROCK can hold it
	ManualReloadF.AddMag(mag, token)
EndFunction

; A magazine put away over the shoulder through ROCK lands in the inventory as an item. Its rounds are
; already back with the ammo you carry, so the item itself is taken away again.
Function Tidy()
	Actor player = Game.GetPlayer()
	int n = player.GetItemCount(MRF_Mags)
	if n > 0
		player.RemoveItem(MRF_Mags, n, true)
	endif
EndFunction

Function PlaySound(int index)
	if index >= 0 && index < MRF_Sounds.GetSize()
		Sound s = MRF_Sounds.GetAt(index) as Sound
		if s
			s.Play(Game.GetPlayer())
		endif
	endif
EndFunction
