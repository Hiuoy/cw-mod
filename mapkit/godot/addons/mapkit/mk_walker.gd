class_name MkWalker
extends CharacterBody3D
## First person at the game's scale and speeds, to check a map's sizes before building it.
## WASD moves, the mouse looks, Shift sprints, Space jumps. Esc frees the mouse; click to take it back.
## Falling off the map puts you back at the start.

const U := MkSpace.UNITS_PER_METER
const WALK := 190.0 / U     # the game's run speed, 190 units/s
const SPRINT := 285.0 / U
const GRAVITY := 800.0 / U  # g_gravity 800
const JUMP := 250.0 / U     # a 39-unit jump
const HEIGHT := 72.0 / U
const EYE := 60.0 / U
const RADIUS := 15.0 / U

var _camera: Camera3D
var _pitch := 0.0
var _start := Transform3D.IDENTITY


func _ready() -> void:
	var capsule := CapsuleShape3D.new()
	capsule.radius = RADIUS
	capsule.height = HEIGHT
	var shape := CollisionShape3D.new()
	shape.shape = capsule
	shape.position.y = HEIGHT / 2
	add_child(shape)
	_camera = Camera3D.new()
	_camera.position.y = EYE
	_camera.fov = 65
	_camera.near = 0.05
	_camera.far = 2000
	add_child(_camera)
	_camera.make_current()
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED


func remember_start() -> void:
	_start = global_transform


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		var motion := event as InputEventMouseMotion
		rotate_y(-motion.relative.x * 0.0025)
		_pitch = clampf(_pitch - motion.relative.y * 0.0025, -1.5, 1.5)
		_camera.rotation.x = _pitch
	elif event is InputEventKey and event.pressed and (event as InputEventKey).physical_keycode == KEY_ESCAPE:
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	elif event is InputEventMouseButton and event.pressed:
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED


func _physics_process(delta: float) -> void:
	var wish := Vector3.ZERO
	if Input.is_physical_key_pressed(KEY_W):
		wish -= global_basis.z
	if Input.is_physical_key_pressed(KEY_S):
		wish += global_basis.z
	if Input.is_physical_key_pressed(KEY_A):
		wish -= global_basis.x
	if Input.is_physical_key_pressed(KEY_D):
		wish += global_basis.x
	wish.y = 0
	wish = wish.normalized()
	var speed := SPRINT if Input.is_physical_key_pressed(KEY_SHIFT) else WALK
	velocity.x = wish.x * speed
	velocity.z = wish.z * speed
	if is_on_floor():
		if Input.is_physical_key_pressed(KEY_SPACE):
			velocity.y = JUMP
	else:
		velocity.y -= GRAVITY * delta
	move_and_slide()
	if global_position.y < _start.origin.y - 100:
		global_transform = _start
		velocity = Vector3.ZERO
