use tauri::WebviewUrl;

fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![open_external, open_webview])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}

#[tauri::command]
fn open_external(url: String) -> Result<(), String> {
    open::that(url).map_err(|e| e.to_string())
}

#[tauri::command]
async fn open_webview(app: tauri::AppHandle, url: String) -> Result<(), String> {
    app.create_webview(
        tauri::WebviewWindowBuilder::new(&app, "browser-webview", WebviewUrl::External(url.parse().map_err(|e| e.to_string())?))
            .title("Browser")
            .inner_size(1280.0, 800.0)
            .build()
            .map_err(|e| e.to_string())?;
    Ok(())
}
