class_name WeaponTuning
extends Resource

enum Rarity {
	Common,
	Uncommon,
	Rare
}

@export var rarity : Rarity
@export var two_handed := false
@export var physical_dmg := 0
@export_range(0, 100) var crit_boost := 0
@export var fire_dmg := 0
@export var ice_dmg := 0
@export var armor_bonus := 0
