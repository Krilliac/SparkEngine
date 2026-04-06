# DataTable System

Data-driven table system for loading, querying, and hot-reloading structured game data from CSV and JSON files.

**Source:** `SparkEngine/Source/Engine/DataTable/DataTableSystem.h`

## Overview

The DataTable system provides a lightweight, schema-flexible way to define game data (items, enemies, levels, balance parameters) in external CSV or JSON files. Tables are loaded at runtime, columns are auto-typed, and rows are queryable by ID or arbitrary column filters.

All values are stored internally as strings and converted on access via typed getters (`GetInt`, `GetFloat`, `GetBool`, `GetString`). Column types (`String`, `Int`, `Float`, `Bool`, `Vector3`) are auto-detected by scanning data values during load. The CSV parser is RFC 4180 compliant, handling quoted fields and embedded commas.

The `DataTableRegistry` singleton manages named tables, supports loading from disk with auto-detection of CSV vs JSON by file extension, and provides hot-reload via `ReloadTable()` which re-reads from the original source path.

## Architecture

```
DataTableRegistry (singleton)
  +-- "weapons"  --> DataTable (columns + rows + index)
  +-- "enemies"  --> DataTable
  +-- "levels"   --> DataTable
  ...

DataTable
  +-- vector<DataColumn>   (name, type, default)
  +-- vector<DataRow>      (string -> string maps)
  +-- unordered_map index  (ID column -> row index)
```

## Key Classes

| Class | Description |
|-------|-------------|
| `DataRow` | A single row of values with typed accessors (`GetInt`, `GetFloat`, etc.) |
| `DataTable` | A collection of rows and columns with CSV/JSON load/save and querying |
| `DataTableRegistry` | Singleton manager for named tables with file loading and hot-reload |
| `DataColumn` | Column definition with name, type, and default value |

## Example Data

### CSV (`data/weapons.csv`)

```csv
id,name,damage,firerate,automatic
pistol,Pistol,25,2.5,false
rifle,Assault Rifle,30,10.0,true
shotgun,Shotgun,80,1.2,false
sniper,Sniper Rifle,100,0.8,false
```

### JSON (`data/enemies.json`)

```json
[
  { "id": "zombie", "name": "Zombie", "health": 100, "speed": 2.5, "aggressive": true },
  { "id": "skeleton", "name": "Skeleton", "health": 60, "speed": 4.0, "aggressive": true }
]
```

## Usage

```cpp
auto& registry = Spark::Data::DataTableRegistry::GetInstance();
registry.Initialize();

// Load from file (auto-detects CSV vs JSON)
registry.LoadTableFromFile("weapons", "data/weapons.csv");

// Query by ID
auto* table = registry.GetTable("weapons");
auto* row = table->GetRow("rifle");
int damage = row->GetInt("damage");       // 30
float rate = row->GetFloat("firerate");   // 10.0f
bool isAuto = row->GetBool("automatic");  // true

// Find all rows matching a value
auto shotguns = table->FindRows("automatic", "false");

// Hot-reload after editing the file
registry.ReloadTable("weapons");

// Validate schema
auto errors = table->Validate();
```

## API Reference

### DataTable

| Method | Description |
|--------|-------------|
| `LoadFromCSV(string)` | Parse CSV content with auto-typed columns |
| `LoadFromJSON(string)` | Parse JSON array of objects |
| `SaveToCSV() / SaveToJSON()` | Export table to string |
| `AddRow(DataRow)` | Add a row (fills defaults, indexes by ID) |
| `RemoveRow(string)` | Remove a row by its ID value |
| `GetRow(string)` | Look up a row by ID column value |
| `FindRows(col, value)` | Find all rows where a column matches a value |
| `Validate()` | Check rows against column type schema |

### DataTableRegistry

| Method | Description |
|--------|-------------|
| `Initialize() / Shutdown()` | Lifecycle management |
| `LoadTableFromFile(name, path)` | Load from disk (CSV/JSON auto-detected) |
| `RegisterTable(name, table)` | Register a pre-built table |
| `GetTable(name)` | Retrieve a table by name |
| `ReloadTable(name)` | Hot-reload from original source file |
| `GetTableNames()` | List all registered table names |

### DataRow

| Method | Description |
|--------|-------------|
| `GetString(col)` | Get string value |
| `GetInt(col)` | Get int value (0 if absent) |
| `GetFloat(col)` | Get float value (0.0f if absent) |
| `GetBool(col)` | Get bool ("true"/"1" = true) |
| `SetValue(col, val)` | Set a column value |

## Configuration

| Option | Description |
|--------|-------------|
| ID column | First column by default, or set via `DataTable("columnName")` constructor |
| Column types | Auto-detected: `Bool`, `Int`, `Float`, `String` |
| Hot-reload | Call `ReloadTable()` -- re-reads from `GetSourcePath()` |

## Related Systems

- [Loot and Crafting System](Loot-And-Crafting-System) -- Uses data tables for item definitions
- [Localization System](Localization-System) -- String tables for translated text
