#!/bin/bash

# SparkEngine Documentation Generation Script
# Generates comprehensive API documentation from header files using Doxygen

set -e  # Exit on any error

# Script configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
DOCS_DIR="$SCRIPT_DIR"
OUTPUT_DIR="$DOCS_DIR/output"
WIKI_DIR="$DOCS_DIR/wiki"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Logging functions
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Function to check if required tools are installed
check_dependencies() {
    log_info "Checking dependencies..."
    
    local missing_deps=()
    
    if ! command -v doxygen &> /dev/null; then
        missing_deps+=("doxygen")
    fi
    
    if ! command -v dot &> /dev/null; then
        missing_deps+=("graphviz")
    fi
    
    if [ ${#missing_deps[@]} -ne 0 ]; then
        log_error "Missing dependencies: ${missing_deps[*]}"
        log_info "Install with: sudo apt install doxygen graphviz"
        exit 1
    fi
    
    log_success "All dependencies found"
}

# Function to clean previous documentation
clean_output() {
    log_info "Cleaning previous documentation..."
    rm -rf "$OUTPUT_DIR"
    rm -rf "$WIKI_DIR"
    mkdir -p "$OUTPUT_DIR"
    mkdir -p "$WIKI_DIR"
    log_success "Cleaned output directories"
}

# Function to generate Doxygen documentation
generate_doxygen() {
    log_info "Generating Doxygen documentation..."
    
    cd "$DOCS_DIR"
    
    # Run Doxygen (uses Doxyfile.txt which indexes SparkEngine, SparkEditor,
    # SparkConsole, SparkShaderCompiler, SparkGame, and SparkSDK sources)
    if doxygen Doxyfile.txt; then
        log_success "Doxygen documentation generated successfully"
    else
        log_error "Doxygen generation failed"
        exit 1
    fi
}

# Function to create main wiki index page
create_wiki_index() {
    log_info "Creating wiki index page..."
    
    local index_file="$WIKI_DIR/index.html"
    
    cat > "$index_file" << 'EOF'
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Spark Engine - Documentation Wiki</title>
    <style>
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            line-height: 1.6;
            margin: 0;
            padding: 0;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: #333;
        }
        .container {
            max-width: 1200px;
            margin: 0 auto;
            padding: 20px;
        }
        .header {
            text-align: center;
            background: rgba(255, 255, 255, 0.95);
            padding: 40px;
            border-radius: 15px;
            margin-bottom: 30px;
            box-shadow: 0 10px 30px rgba(0, 0, 0, 0.2);
        }
        .header h1 {
            color: #2c3e50;
            margin: 0;
            font-size: 2.5em;
            font-weight: 300;
        }
        .header p {
            color: #7f8c8d;
            font-size: 1.2em;
            margin: 10px 0 0 0;
        }
        .nav-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
            gap: 20px;
            margin-bottom: 30px;
        }
        .nav-card {
            background: rgba(255, 255, 255, 0.95);
            padding: 25px;
            border-radius: 10px;
            text-decoration: none;
            color: inherit;
            transition: transform 0.3s ease, box-shadow 0.3s ease;
            box-shadow: 0 5px 15px rgba(0, 0, 0, 0.1);
        }
        .nav-card:hover {
            transform: translateY(-5px);
            box-shadow: 0 10px 25px rgba(0, 0, 0, 0.2);
        }
        .nav-card h3 {
            color: #3498db;
            margin-top: 0;
            margin-bottom: 10px;
        }
        .nav-card p {
            color: #7f8c8d;
            margin: 0;
        }
        .features {
            background: rgba(255, 255, 255, 0.95);
            padding: 30px;
            border-radius: 10px;
            box-shadow: 0 5px 15px rgba(0, 0, 0, 0.1);
        }
        .features h2 {
            color: #2c3e50;
            margin-top: 0;
        }
        .feature-list {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
            gap: 15px;
            margin-top: 20px;
        }
        .feature-item {
            padding: 15px;
            background: #f8f9fa;
            border-radius: 8px;
            border-left: 4px solid #3498db;
        }
        .feature-item h4 {
            margin: 0 0 8px 0;
            color: #2c3e50;
        }
        .feature-item p {
            margin: 0;
            color: #7f8c8d;
            font-size: 0.9em;
        }
        .footer {
            text-align: center;
            margin-top: 40px;
            padding: 20px;
            color: rgba(255, 255, 255, 0.8);
        }
        .auto-update-info {
            background: rgba(52, 152, 219, 0.1);
            border: 1px solid #3498db;
            border-radius: 8px;
            padding: 15px;
            margin: 20px 0;
        }
        .auto-update-info h4 {
            color: #3498db;
            margin-top: 0;
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>🔥 Spark Engine</h1>
            <p>Comprehensive API Documentation & Developer Wiki</p>
        </div>

        <div class="nav-grid">
            <a href="output/index.html" class="nav-card">
                <h3>📚 Full API Documentation</h3>
                <p>Complete Doxygen-generated API reference with class diagrams, call graphs, and detailed function documentation.</p>
            </a>
            
            <a href="output/modules.html" class="nav-card">
                <h3>🧩 Module Overview</h3>
                <p>Browse engine modules: Audio, Graphics, Physics, Input, Core systems, and more.</p>
            </a>
            
            <a href="output/classes.html" class="nav-card">
                <h3>🏗️ Class Reference</h3>
                <p>Alphabetical index of all classes, structures, and interfaces in the engine.</p>
            </a>
            
            <a href="output/files.html" class="nav-card">
                <h3>📁 File Browser</h3>
                <p>Browse source files with syntax highlighting and cross-references.</p>
            </a>
        </div>

        <div class="features">
            <h2>🌟 Engine Features</h2>
            <div class="feature-list">
                <div class="feature-item">
                    <h4>🎨 DirectX 11 Rendering</h4>
                    <p>Modern graphics pipeline with dynamic lighting, shadows, and post-processing effects</p>
                </div>
                <div class="feature-item">
                    <h4>🎮 ECS Architecture</h4>
                    <p>Entity-Component-System design for modular and efficient game object management</p>
                </div>
                <div class="feature-item">
                    <h4>🔊 XAudio2 Sound</h4>
                    <p>3D spatial audio with comprehensive console integration and real-time controls</p>
                </div>
                <div class="feature-item">
                    <h4>⚡ AngelScript</h4>
                    <p>Hot-reload scripting system with full debugging support</p>
                </div>
                <div class="feature-item">
                    <h4>🛠️ Visual Editor</h4>
                    <p>ImGui-powered editor with scene hierarchy, inspector, and asset browser</p>
                </div>
                <div class="feature-item">
                    <h4>🩹 Crash Handling</h4>
                    <p>Automatic minidumps with symbolized stack traces for debugging</p>
                </div>
            </div>

            <div class="auto-update-info">
                <h4>🔄 Auto-Updated Documentation</h4>
                <p>This documentation is automatically regenerated when source code changes are detected. 
                   The wiki stays synchronized with the latest engine features and API changes.</p>
            </div>
        </div>

        <div class="footer">
            <p>Documentation generated automatically from source code comments</p>
            <p>Last updated: <span id="lastUpdate"></span></p>
        </div>
    </div>

    <script>
        document.getElementById('lastUpdate').textContent = new Date().toLocaleString();
    </script>
</body>
</html>
EOF
    
    log_success "Wiki index page created"
}

# Function to copy and enhance Doxygen output
enhance_documentation() {
    log_info "Enhancing documentation..."
    
    # Copy Doxygen HTML output to wiki directory for easy access
    if [ -d "$OUTPUT_DIR/html" ]; then
        mkdir -p "$WIKI_DIR/output"
        cp -r "$OUTPUT_DIR/html"/* "$WIKI_DIR/output/"
        log_success "Doxygen output copied to wiki"
    else
        log_error "Doxygen HTML output not found"
        exit 1
    fi
}

# Function to generate module-specific pages
generate_module_pages() {
    log_info "Generating module-specific documentation..."
    
    local modules=("Audio" "Graphics" "Core" "Input" "Physics" "Camera" "Utils" "Game" "Engine" "AI" "Animation" "Networking" "Scripting" "ECS" "SaveSystem")
    
    for module in "${modules[@]}"; do
        local module_file="$WIKI_DIR/${module,,}.html"
        
        cat > "$module_file" << EOF
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Spark Engine - $module Module</title>
    <style>
        body { font-family: 'Segoe UI', sans-serif; margin: 0; padding: 20px; background: #f5f7fa; }
        .container { max-width: 1000px; margin: 0 auto; }
        .header { background: white; padding: 30px; border-radius: 10px; margin-bottom: 20px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
        .back-link { color: #3498db; text-decoration: none; margin-bottom: 20px; display: inline-block; }
        .back-link:hover { text-decoration: underline; }
        h1 { color: #2c3e50; margin: 0; }
        .description { color: #7f8c8d; margin-top: 10px; }
        iframe { width: 100%; height: 600px; border: none; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
    </style>
</head>
<body>
    <div class="container">
        <a href="index.html" class="back-link">← Back to Wiki Home</a>
        <div class="header">
            <h1>$module Module Documentation</h1>
            <p class="description">Detailed API reference for the $module module of Spark Engine</p>
        </div>
        <iframe src="output/index.html" title="$module Module API Documentation"></iframe>
    </div>
</body>
</html>
EOF
    done
    
    log_success "Module pages generated"
}

# Function to display summary
show_summary() {
    log_success "Documentation generation completed!"
    echo
    log_info "Generated files:"
    echo "  📁 Main wiki: $WIKI_DIR/index.html"
    echo "  📚 Full docs: $WIKI_DIR/output/html/index.html"
    echo "  🗂️ Module pages: $WIKI_DIR/{audio,graphics,core,etc}.html"
    echo
    log_info "To view the documentation:"
    echo "  1. Open $WIKI_DIR/index.html in a web browser"
    echo "  2. Or serve with: cd $WIKI_DIR && python3 -m http.server 8000"
    echo "  3. Then visit: http://localhost:8000"
    echo
    log_info "To update documentation, run this script again or use the auto-update script"
}

# Main execution
main() {
    log_info "Starting Spark Engine documentation generation..."
    echo "======================================================"
    
    check_dependencies
    clean_output
    generate_doxygen
    create_wiki_index
    enhance_documentation
    generate_module_pages
    show_summary
    
    echo "======================================================"
    log_success "Documentation generation completed successfully!"
}

# Run main function if script is executed directly
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi