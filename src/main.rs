use slint::ComponentHandle;

slint::include_modules!();

fn main() -> Result<(), Box<dyn std::error::Error>> {
    slint::BackendSelector::new()
        .backend_name("winit".into())
        .renderer_name("femtovg".into())
        .require_opengl_es_with_version(3, 0)
        .select()?;

    let ui = MainWindow::new()?;
    ui.window().set_fullscreen(true);
    ui.run()?;
    Ok(())
}
