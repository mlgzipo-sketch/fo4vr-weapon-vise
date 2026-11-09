Scriptname Actor extends ObjectReference Native
Function EquipItem(Form akItem, bool abPreventRemoval = false, bool abSilent = false) native
Weapon Function GetEquippedWeapon(int aiEquipIndex = 0) native
bool Function IsWeaponDrawn() native
Function UnequipItem(Form akItem, bool abPreventEquip = false, bool abSilent = false) native
Event OnPlayerLoadGame()
EndEvent
