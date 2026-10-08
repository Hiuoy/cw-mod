@tool
class_name MkBrush
extends CSGBox3D
## A block of level geometry: floors, walls, ramps, platforms. cwlink turns it into collision and a
## rendered surface. Plain CSGBox3D nodes export too, with the defaults below.
## Only boxes in union mode export for now; subtraction and other CSG shapes are reported by Check.

const GRID_MATERIAL := preload("res://addons/mapkit/brush_grid.tres")

## The game material to draw it with, by the game's name for it. Empty: the default material.
@export var game_material: String = ""
## Players and zombies collide with it.
@export var solid: bool = true
## Drawn in the game. Off: an invisible wall that still blocks (a clip).
@export var rendered: bool = true


func _ready() -> void:
	if material == null:
		material = GRID_MATERIAL
