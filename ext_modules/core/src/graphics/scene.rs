use pyo3::prelude::*;

#[pyclass]
#[derive(Debug, Clone, Default)]
pub struct Scene {
    #[pyo3(get, set)]
    pub viewport: Viewport,
    pub camera: Camera,
    pub surfaces: Vec<Surface>,
}

#[pymethods]
impl Scene {
    #[new]
    pub fn new() -> Self {
        Self::default()
    }

    #[getter]
    pub fn get_camera(&self, py: Python<'_>) -> PyObject {
        match self.camera.clone() {
            Camera::Rotate(camera) => camera.into_py(py),
            Camera::BirdsEye(camera) => camera.into_py(py),
        }
    }

    #[setter]
    pub fn set_camera(&mut self, camera: &PyAny) -> PyResult<()> {
        if let Ok(rotate_camera) = camera.extract::<RotateCamera>() {
            self.camera = Camera::Rotate(rotate_camera);
        } else {
            self.camera = Camera::BirdsEye(camera.extract()?);
        }
        Ok(())
    }
}

#[pyclass]
#[derive(Debug, Clone, Default)]
pub struct Viewport {
    #[pyo3(get, set)]
    pub x: f32,
    #[pyo3(get, set)]
    pub y: f32,
    #[pyo3(get, set)]
    pub width: f32,
    #[pyo3(get, set)]
    pub height: f32,
}

#[pymethods]
impl Viewport {
    #[new]
    pub fn new() -> Self {
        Self::default()
    }
}

#[derive(Debug, Clone)]
pub enum Camera {
    Rotate(RotateCamera),
    BirdsEye(BirdsEyeCamera),
}

impl Default for Camera {
    fn default() -> Self {
        Self::BirdsEye(BirdsEyeCamera::default())
    }
}

#[pyclass]
#[derive(Debug, Clone, Default)]
pub struct RotateCamera {
    #[pyo3(get, set)]
    pub pos: [f32; 3],
    #[pyo3(get, set)]
    pub target: [f32; 3],
    #[pyo3(get, set)]
    pub pitch: f32, // TODO: Can compute pitch and yaw from target
    #[pyo3(get, set)]
    pub yaw: f32,
    #[pyo3(get, set)]
    pub fov_y: f32,
}

#[pymethods]
impl RotateCamera {
    #[new]
    pub fn new() -> Self {
        Self::default()
    }
}

#[pyclass]
#[derive(Debug, Clone, Default)]
pub struct BirdsEyeCamera {
    #[pyo3(get, set)]
    pub pos: [f32; 3],
    #[pyo3(get, set)]
    pub span_y: f32,
}

#[pymethods]
impl BirdsEyeCamera {
    #[new]
    pub fn new() -> Self {
        Self::default()
    }
}

#[derive(Debug, Clone)]
pub struct Surface {
    pub ty: SurfaceType,
    pub vertices: [[f32; 3]; 3],
    pub normal: [f32; 3],
}

#[derive(Debug, Clone, Copy)]
pub enum SurfaceType {
    Floor,
    Ceiling,
    WallXProj,
    WallZProj,
}
