use tauri::WebviewUrl;

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_fs::init())
        .invoke_handler(tauri::generate_handler![open_external, open_webview, save_page])
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

#[tauri::command]
async fn save_page(app: tauri::AppHandle, url: String) -> Result<(), String> {
    use std::io::Write;
    use tauri_plugin_dialog::DialogExt;
    use tauri_plugin_fs::FsExt;

    let file_path = app.dialog().file_picker().save_file().await.map_err(|e| e.to_string())?;
    let path = file_path.to_string();
    if path.is_empty() {
        return Ok(());
    }

    let client = reqwest::blocking::Client::builder()
        .user_agent("AIBrowser/1.0")
        .build()
        .map_err(|e| e.to_string())?;
    let resp = client.get(&url).send().map_err(|e| e.to_string())?;
    let bytes = resp.bytes().map_err(|e| e.to_string())?;

    app.fs().write_file(path.clone(), bytes.to_vec()).await.map_err(|e| e.to_string())?;
    Ok(())
}
