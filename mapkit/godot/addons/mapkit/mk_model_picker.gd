@tool
extends ConfirmationDialog
## The model picker: the game's models by category, each with its picture, to choose one for a model field
## (mk_inspector.gd opens it). The list comes from mkasset (MkAssets.catalog) and the names from
## MkModelCatalog; the pictures are drawn as they scroll into view and cached (MkAssets.thumbnail).

## The chosen model: its name, else #<hash>.
signal picked(model: String)

## Pictures being drawn at once: enough to keep mkasset busy, few enough that the editor stays smooth.
const DRAWING := 3
const THUMBNAIL_COLUMN := 144

var _category: OptionButton
var _search: LineEdit
var _list: ItemList
var _info: Label
var _entries: Array[Dictionary] = []
var _shown: Array[Dictionary] = []  # the entries in the list, in its order
var _row_of := {}                   # key -> its row in the list
var _asked := {}                    # key -> true: its picture was asked for
var _drawing := 0
var _current := ""
var _placeholder: Texture2D


func _init() -> void:
	title = "Pick a game model"
	ok_button_text = "Use this model"
	get_ok_button().disabled = true
	var box := VBoxContainer.new()
	var bar := HBoxContainer.new()
	_category = OptionButton.new()
	for category in MkModelCatalog.categories():
		_category.add_item(category)
	_category.item_selected.connect(func(_index: int) -> void: _filter())
	bar.add_child(_category)
	_search = LineEdit.new()
	_search.placeholder_text = "Search names and hashes"
	_search.clear_button_enabled = true
	_search.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_search.text_changed.connect(func(_text: String) -> void: _filter())
	bar.add_child(_search)
	box.add_child(bar)
	_list = ItemList.new()
	_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_list.icon_mode = ItemList.ICON_MODE_TOP
	_list.max_columns = 0
	_list.same_column_width = true
	_list.fixed_column_width = THUMBNAIL_COLUMN
	_list.fixed_icon_size = Vector2i(MkAssets.THUMBNAIL_SIZE, MkAssets.THUMBNAIL_SIZE)
	_list.max_text_lines = 2
	_list.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	_list.item_selected.connect(_selected)
	_list.item_activated.connect(func(_row: int) -> void:
		_confirm()
		hide())
	box.add_child(_list)
	_info = Label.new()
	_info.text = "Reading the game's model list..."
	box.add_child(_info)
	add_child(box)
	confirmed.connect(_confirm)
	var blank := Image.create_empty(MkAssets.THUMBNAIL_SIZE, MkAssets.THUMBNAIL_SIZE, false, Image.FORMAT_RGBA8)
	_placeholder = ImageTexture.create_from_image(blank)


## Lists the models, starting on a category ("" = All; locked when the field only takes that category) with the
## field's current model selected when it is in the list. Call it once the dialog shows.
func open(category: String, locked: bool, current: String) -> void:
	_current = MkAssets.model_key(current)
	for i in _category.item_count:
		if _category.get_item_text(i) == category:
			_category.select(i)
	_category.disabled = locked and not category.is_empty()
	MkAssets.catalog(_listed)
	# Gone once closed. Connected only now: a window also changes visibility while it pops up.
	visibility_changed.connect(func() -> void:
		if not visible:
			queue_free())


func _listed(catalog: Dictionary) -> void:
	if not is_inside_tree():
		return
	_entries = MkModelCatalog.entries(catalog)
	if _entries.is_empty():
		_info.text = "No game models: see the Output panel (mkasset needs the game folder and a zone trace)."
		return
	_filter()
	if _row_of.has(_current):
		_list.select(_row_of[_current])
		_list.ensure_current_is_visible()
		_selected(_row_of[_current])
	_search.grab_focus()


func _filter() -> void:
	var category := _category.get_item_text(_category.selected)
	var words := _search.text.to_lower().split(" ", false)
	_list.clear()
	_shown.clear()
	_row_of.clear()
	for entry in _entries:
		if category != MkModelCatalog.ALL and entry.category != category:
			continue
		var text: String = (entry.name + " " + entry.key).to_lower()
		if not _has_all(text, words):
			continue
		var row := _list.add_item(_label(entry), MkAssets.cached_thumbnail(entry.key, _placeholder))
		_list.set_item_tooltip(row, _describe(entry))
		_row_of[entry.key] = row
		_shown.append(entry)
	_info.text = "%d models. Pictures are drawn as they come into view (the first time only)." % _shown.size()
	get_ok_button().disabled = true


static func _has_all(text: String, words: PackedStringArray) -> bool:
	for word in words:
		if not word in text:
			return false
	return true


func _label(entry: Dictionary) -> String:
	if entry.name.is_empty():
		return "#" + entry.key
	# The common prefixes say little in a small label (the tooltip has the full name).
	var name: String = entry.name
	for prefix in ["p9_zm_", "p8_zm_", "p7_zm_", "p9_", "p8_", "p7_"]:
		if name.begins_with(prefix):
			return name.substr(prefix.length())
	return name


func _describe(entry: Dictionary) -> String:
	var size: Vector3 = entry.size
	return "%s\n#%s, in %s\n%d x %d x %d units (length, width, height), %d triangles" % [
		entry.name if not entry.name.is_empty() else "(no name known)", entry.key, entry.zone,
		roundi(size.x), roundi(size.y), roundi(size.z), entry.triangles]


func _selected(row: int) -> void:
	if row < 0 or row >= _shown.size():
		return
	_info.text = _describe(_shown[row]).replace("\n", "   ")
	get_ok_button().disabled = false


func _confirm() -> void:
	var rows := _list.get_selected_items()
	if not rows.is_empty():
		picked.emit(MkModelCatalog.field_value(_shown[rows[0]]))


func _process(_delta: float) -> void:
	if not visible or _shown.is_empty() or _drawing >= DRAWING:
		return
	# The rows in view, then a screen ahead.
	var first := maxi(_list.get_item_at_position(Vector2(8, 8), false), 0)
	var columns := maxi(int(_list.size.x / (THUMBNAIL_COLUMN + 8)), 1)
	var rows_in_view := int(_list.size.y / (MkAssets.THUMBNAIL_SIZE + 40)) + 2
	var last := mini(first + columns * rows_in_view * 2, _shown.size())
	for row in range(first, last):
		var key: String = _shown[row].key
		if _asked.has(key) or MkAssets.cached_thumbnail(key) != null:
			continue
		_asked[key] = true
		_drawing += 1
		MkAssets.thumbnail(key, _drawn.bind(key))
		if _drawing >= DRAWING:
			return


func _drawn(picture: Texture2D, key: String) -> void:
	_drawing -= 1
	if not is_inside_tree() or not _row_of.has(key):
		return
	var row: int = _row_of[key]
	if picture != null:
		_list.set_item_icon(row, picture)
	else:
		_list.set_item_custom_fg_color(row, Color(1, 1, 1, 0.45))
