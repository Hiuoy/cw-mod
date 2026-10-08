@tool
extends EditorInspectorPlugin
## The model fields of mapkit's objects: a picture of the model, its name, and Pick, which opens the model picker
## (mk_model_picker.gd).
##
## A model field is a String export with the hint string "mk_model", optionally ":<category>" (the picker starts
## on it) and ":<prefix>" (the field holds the model's name without it, and the picker keeps to the category),
## for example @export_custom(PROPERTY_HINT_NONE, "mk_model:Wall buys:p9_zm_chalk_buy_"). Without the plugin it
## is a plain text field.

const Picker := preload("res://addons/mapkit/mk_model_picker.gd")


func _can_handle(object: Object) -> bool:
	return object is MkEntity


func _parse_property(_object: Object, type: Variant.Type, name: String, _hint_type: PropertyHint, hint_string: String,
		_usage_flags: int, _wide: bool) -> bool:
	if type != TYPE_STRING or not (hint_string == "mk_model" or hint_string.begins_with("mk_model:")):
		return false
	add_property_editor(name, ModelField.new(hint_string))
	return true


class ModelField extends EditorProperty:
	var _category := ""
	var _prefix := ""
	var _picture: TextureRect
	var _edit: LineEdit
	var _shown := ""

	func _init(hint_string: String) -> void:
		var parts := hint_string.split(":")
		_category = parts[1] if parts.size() > 1 else ""
		_prefix = parts[2] if parts.size() > 2 else ""
		var row := HBoxContainer.new()
		_picture = TextureRect.new()
		_picture.custom_minimum_size = Vector2(48, 48)
		_picture.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
		_picture.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
		row.add_child(_picture)
		_edit = LineEdit.new()
		_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_edit.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		_edit.text_submitted.connect(func(_text: String) -> void: _commit())
		_edit.focus_exited.connect(_commit)
		row.add_child(_edit)
		var pick := Button.new()
		pick.text = "Pick"
		pick.tooltip_text = "Choose from the game's models, with pictures"
		pick.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		pick.pressed.connect(_pick)
		row.add_child(pick)
		add_child(row)
		add_focusable(_edit)
		add_focusable(pick)

	func _update_property() -> void:
		var value: String = get_edited_object()[get_edited_property()]
		if not _edit.has_focus():
			_edit.text = value
		_show(_model_of(value))

	func _commit() -> void:
		var value := _edit.text.strip_edges()
		if value != get_edited_object()[get_edited_property()]:
			emit_changed(get_edited_property(), value)

	func _pick() -> void:
		var picker := Picker.new()
		picker.picked.connect(func(model: String) -> void:
			emit_changed(get_edited_property(), _value_of(model)))
		EditorInterface.popup_dialog_centered_clamped(picker, Vector2i(960, 680), 0.8)
		picker.open(_category, not _prefix.is_empty(), _model_of(get_edited_object()[get_edited_property()]))

	## The model a field value names.
	func _model_of(value: String) -> String:
		return _prefix + value if not value.is_empty() else ""

	## The field value for a model.
	func _value_of(model: String) -> String:
		return model.substr(_prefix.length()) if not _prefix.is_empty() and model.begins_with(_prefix) else model

	func _show(model: String) -> void:
		if model == _shown:
			return
		_shown = model
		_picture.texture = null
		if not model.is_empty():
			MkAssets.thumbnail(model, _pictured.bind(model))

	func _pictured(picture: Texture2D, model: String) -> void:
		if model == _shown:
			_picture.texture = picture
