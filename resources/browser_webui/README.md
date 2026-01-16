# Chrome WebUI  

This directory contains the **WebUI resources** for the custom Chrome browser.  
It is built on top of Chrome’s WebUI components.  

The WebUI is responsible for rendering the browser’s user interface, including the toolbar and other UI elements.  

---

## Key Files and Folders  

1. **`index.html` / `index.ts`**  
   The main entry point for the WebUI.  

2. **`ui_store.ts`**  
   Manages UI mode state and layout preferences.  

3. **`controller/tabs_controller.ts`**  
   Handles tab state management and tab-related events.  

4. **`pages/`**  
   Contains the individual pages for different page modes.  

5. **`components/`**  
   Contains reusable UI components.  

---

## Components  

There are two kinds of components you can use in this WebUI:  

### 1. Native View Holder  

The `native_view_holder.ts` component is specific to this WebUI.  
It allows embedding **native views** by setting a class name or ID.  

- The `native-view-holder` participates in layout and visibility updates.  
- When its state changes, it automatically synchronizes with the corresponding native view.  

#### Example  

```html
<native-view-holder
    view-class="MainContentsView"
    align-x="stretch"
    align-y="stretch">
</native-view-holder>

<native-view-holder
    view-class="LocationBarView"
    align-x="stretch"
    align-y="top"
    extra-style="address_box_expand_on_focus">
</native-view-holder>

<native-view-holder
    view-id="1001"
    align-x="left"
    align-y="top">
</native-view-holder>
```

#### Special Views  

- **`MainContentsView`**: The main browser container. Displays tab contents and DevTools.  
- **`LocationBarView`**: The browser's address bar. Use `extra-style="address_box_expand_on_focus"` to auto-expand while focused.  
- **`BackButton`**: Standard back navigation.  
- **`ForwardButton`**: Standard forward navigation.  
- **`ReloadButton`**: Reloads the current page.  
- **`HomeButton`**: Navigates to the home page.  
- **`BrowserAppMenuButton`**: Opens the browser app menu.  
- **`AvatarToolbarButton`**: Displays the profile/avatar menu.  
- **`ChromeLabsButton`**: Opens Chrome Labs experiments.  
- **`ExtensionsMenuButton`**: Opens the extensions menu.  
- **`MediaButton`**: Opens the media control dialog.  
- **`NewTabButton`**: Opens a new tab.  
- **`SidePanelButton`**: Toggles the side panel.  
- **`PinnedClearBrowsingDataButton`**: Opens “Clear browsing data” dialog.  
- **`PinnedCopyLinkButton`**: Copies the current URL to the clipboard.  
- **`PinnedDeveloperToolsButton`**: Opens DevTools.  
- **`PinnedNewIncognitoWindowButton`**: Opens a new incognito window.  
- **`PinnedPrintButton`**: Print command button.  
- **`PinnedQrCodeGeneratorButton`**: Generates a QR code for the current page.  
- **`PinnedCastButton`**: Casts media to another device.  
- **`PinnedSendTabToSelfButton`**: Sends the current tab to another device.  
- **`PinnedShowAddressesBubbleOrPageButton`**: Shows saved addresses.  
- **`PinnedShowChromeLabsButton`**: Opens Chrome Labs via pinned button.  
- **`PinnedShowDownloadsButton`**: Opens the Downloads page.  
- **`PinnedShowPasswordsBubbleOrPageButton`**: Opens the Passwords manager.  
- **`PinnedShowPaymentsBubbleOrPageButton`**: Opens the Payments/Autofill manager.  
- **`PinnedShowTranslateButton`**: Opens the Translate bubble.  
- **`PinnedShowBookmarkSidePanelButton`**: Shows the Bookmarks side panel.  
- **`PinnedShowReadAnythingSidePanelButton`**: Opens the “Read Anything” side panel.  
- **`PinnedShowHistorySidePanelButton`**: Opens the History side panel.  
- **`PinnedShowReadingListSidePanelButton`**: Opens the Reading List side panel.  
- **`PinnedShowSearchCompanionSidePanelButton`**: Opens Search Companion side panel.  
- **`PinnedTaskManagerButton`**: Opens Task Manager.  
- **`PinnedShowLensOverlayResultsSidePanelButton`**: Opens Google Lens overlay results.  
- **`SharedTabGroupFeedbackButton`**: Feedback for shared tab groups.  
- **`PinnedTabSearchButton`**: Opens the tab search UI.  


#### Special Native View IDs  

- `1001` – BackButton  
- `1002` – ForwardButton  
- `1003` – ReloadButton  
- `1004` – HomeButton  
- `1007` – BrowserAppMenuButton  
- `1018` – AvatarToolbarButton  

---

### 2. Standard Chrome Web Components  

In addition to custom native view holders, the WebUI also supports Chrome’s built-in Web Components (e.g., `<cr-icon-button>` and others).  

### 3. Component Organization

To keep the project organized:

- Common Components
Place components that are reused across multiple page modes directly in the components/ root directory.

- Page-Mode Specific Components
If a component is only relevant to a particular page mode, create a subdirectory named after that mode inside components/, and place the component there.
