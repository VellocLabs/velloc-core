# Custom Chrome WebUI: Pages  

This directory contains the **individual page definitions** for different browser UI modes.  

## Page Files  

1. **`vertical/vertical_page.ts`**  
   Implements a **sidebar-based layout** inspired by Arc/Zen Browser.  

2. **`vertical_compact/vertical_compact_page.ts`**  
   Implements a **compact vertical layout** with hover-revealed side controls.  

3. **`horizontal/README.md`**  
   Placeholder for horizontal mode pages.  

---

## Vertical Page  

The **`vertical/vertical_page.ts`** defines a modern, minimal UI where most controls are centralized in a **vertical sidebar**:  

### 1. Sidebar Toolbar (Left Panel)  
A vertical panel laid out from top to bottom. It contains:  

1. **Sidebar Top Bar** (fixed height, aligned top)  
   - **Left-aligned buttons**: `BrowserAppMenuButton`, `HomeButton`  
   - **Right-aligned buttons**: `BackButton`, `ForwardButton`, `ReloadButton`  

2. **Sidebar Address Bar** (fixed height)  
   - Contains the browser’s address bar.  

3. **Sidebar Tab List Bar** (stretchable height, scrollable)  
   - Displays the tab strip as a vertical list.  

4. **Sidebar Bottom Bar** (fixed height, aligned bottom)  
   - **Left-aligned buttons**: `DownloadButton`  
   - **Right-aligned buttons**: `NewTab`  

---

### 2. Main Content Area (Right Panel)  
- Displays the **page contents**, such as active tab content or DevTools.  
- Corresponds to the `MainContentsView`.  
- The `MainContentsView` should have **margins around the edges** (i.e., do not place content flush against the window borders).  

---

### 3. Top Area (Title Bar / Window Controls)  
- The title bar and window controls may behave dynamically:  
  - **Appear only on hover**, or  
  - Remain **hidden** for a cleaner look.  
