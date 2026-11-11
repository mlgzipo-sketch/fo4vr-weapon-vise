Scriptname ManualReloadMain extends Quest
{Manual reload: the game side. ManualReload.dll watches the hands and the gun and sends
commands; this script spawns and removes magazines, makes the ghost models and plays sounds.}

FormList Property MR_Mags Auto Const       ; magazine items by kind: 0 standard, 1 large
FormList Property MR_Ghosts Auto Const     ; outline models, same order
FormList Property MR_Sounds Auto Const     ; 0 snap in, 1 drop out, 2 slide
EffectShader Property Outline Auto Const

int Property CmdSpawn = 1 AutoReadOnly
int Property CmdDelete = 2 AutoReadOnly
int Property CmdSound = 3 AutoReadOnly
int Property CmdGhosts = 4 AutoReadOnly
int Property TimerPoll = 1 AutoReadOnly

ObjectReference[] Ghosts
bool Busy

bool Function Ready()
	return MR_Mags && MR_Ghosts && MR_Sounds
EndFunction

Event OnInit()
	Setup()
EndEvent

Function Setup()
	RegisterForRemoteEvent(Game.GetPlayer(), "OnPlayerLoadGame")
	StartTimer(0.1, TimerPoll)
EndFunction

Event Actor.OnPlayerLoadGame(Actor akSender)
	StartTimer(0.1, TimerPoll)
EndEvent

Event OnTimer(int aiTimerID)
	if !Ready()
		return
	endif
	if !Busy
		Busy = true
		Handle()
		Busy = false
	endif
	StartTimer(0.1, TimerPoll)
EndEvent

Function Handle()
	int cmd = ManualReload.PollCommand()
	int n = 0
	while cmd != 0 && n < 8
		if cmd == CmdSpawn
			Spawn(ManualReload.CommandKind(), ManualReload.CommandPose(), ManualReload.CommandToken())
		elseif cmd == CmdDelete
			ObjectReference ref = Game.GetForm(ManualReload.CommandRefID()) as ObjectReference
			if ref
				ref.Disable()
				ref.Delete()
			endif
		elseif cmd == CmdSound
			PlaySound(ManualReload.CommandKind())
		elseif cmd == CmdGhosts
			MakeGhosts()
		endif
		n += 1
		cmd = ManualReload.PollCommand()
	endwhile
EndFunction

bool Function WaitFor3D(ObjectReference ref)
	int n = 0
	while ref && !ref.Is3DLoaded() && n < 40
		Utility.Wait(0.05)
		n += 1
	endwhile
	return ref && ref.Is3DLoaded()
EndFunction

Function Spawn(int kind, float[] pose, int token)
	if kind < 0 || kind >= MR_Mags.GetSize()
		kind = 0
	endif
	ObjectReference mag = Game.GetPlayer().PlaceAtMe(MR_Mags.GetAt(kind), 1, true, true, true)
	if !mag
		ManualReload.Notify(1)
		return
	endif
	mag.SetPosition(pose[0], pose[1], pose[2])
	mag.SetAngle(pose[3], pose[4], pose[5])
	mag.Enable()
	WaitFor3D(mag)
	mag.SetMotionType(1, true)   ; a normal physical object: it falls, ROCK can hold it
	ManualReload.AddMag(mag, token)
EndFunction

; One outline model per magazine kind, made once and kept in the save; the plugin places and hides them.
Function MakeGhosts()
	if !Ghosts
		Ghosts = new ObjectReference[0]
	endif
	int i = 0
	while i < MR_Ghosts.GetSize()
		ObjectReference g = None
		if i < Ghosts.Length
			g = Ghosts[i]
		endif
		if !g || g.IsDeleted()
			g = Game.GetPlayer().PlaceAtMe(MR_Ghosts.GetAt(i), 1, true, false, false)
			if g
				WaitFor3D(g)
				if Outline
					Outline.Play(g, -1.0)
				endif
				if i < Ghosts.Length
					Ghosts[i] = g
				else
					Ghosts.Add(g)
				endif
			endif
		elseif g.GetDistance(Game.GetPlayer()) > 300.0
			; models far from their reference get culled by the renderer: keep it with the player
			g.MoveTo(Game.GetPlayer())
			WaitFor3D(g)
		endif
		if g
			ManualReload.AddGhost(g, i)
		endif
		i += 1
	endwhile
EndFunction

Function PlaySound(int index)
	if index >= 0 && index < MR_Sounds.GetSize()
		Sound s = MR_Sounds.GetAt(index) as Sound
		if s
			s.Play(Game.GetPlayer())
		endif
	endif
EndFunction
