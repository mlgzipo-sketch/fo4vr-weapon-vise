Scriptname RigVRItem extends ObjectReference
{On the mannequin kit and the pouches: dropping one from the Pip-Boy sets the mannequin up / puts the pouch back.}

RigVRMain Property Main Auto Const
int Property Kind Auto Const   ; 0 the mannequin kit, 1..6 a pouch

Event OnContainerChanged(ObjectReference akNewContainer, ObjectReference akOldContainer)
	if akNewContainer == None && akOldContainer == Game.GetPlayer() && Main
		Main.ItemDropped(self, Kind)
	endif
EndEvent
