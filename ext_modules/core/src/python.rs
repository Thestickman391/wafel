//! SM64-specific Python API for Wafel.
//!
//! The exposed API is **not** safe because of the assumptions made about DLL loading.
use crate::{
    dll,
    error::{Error, ErrorCause},
    memory::{Memory, Value},
    sm64::{
        load_dll_pipeline, object_behavior, object_path, ObjectBehavior, ObjectSlot, Pipeline,
        SM64ErrorCause, SurfaceSlot, Variable,
    },
    timeline::{SlotState, State},
};
use derive_more::Display;
use lazy_static::lazy_static;
use pyo3::{
    basic::CompareOp,
    prelude::*,
    types::{PyFloat, PyLong},
    PyObjectProtocol,
};
use std::{
    collections::{hash_map::DefaultHasher, HashMap},
    fmt::Debug,
    hash::{Hash, Hasher},
    sync::Mutex,
};

// TODO: __str__, __repr__, __eq__, __hash__ for PyVariable, PyObjectBehavior, PyAddress

#[pymodule]
fn core(_py: Python<'_>, m: &PyModule) -> PyResult<()> {
    m.add_class::<PyPipeline>()?;
    m.add_class::<PyVariable>()?;
    m.add_class::<PyObjectBehavior>()?;
    m.add_class::<PyAddress>()?;
    Ok(())
}

#[allow(missing_docs)]
mod wafel_error {
    use pyo3::{create_exception, exceptions::Exception};

    create_exception!(wafel, WafelError, Exception);
}
use wafel_error::*;

impl From<Error> for PyErr {
    fn from(err: Error) -> PyErr {
        PyErr::new::<WafelError, _>(err.to_string())
    }
}

const NUM_BACKUP_SLOTS: usize = 20;

lazy_static! {
    static ref VALID_PIPELINES: Mutex<Vec<Py<PyPipeline>>> = Mutex::new(Vec::new());
}

/// An abstraction for reading and writing variables.
///
/// Note that writing a value to a variable and then reading the variable does not
/// necessarily result in the original value.
#[pyclass(name = Pipeline, unsendable)]
#[derive(Debug)]
pub struct PyPipeline {
    valid: Option<ValidPipeline>,
}

#[derive(Debug)]
struct ValidPipeline {
    pipeline: Pipeline<dll::Memory>,
    symbols_by_address: HashMap<dll::Address, String>,
}

impl PyPipeline {
    fn new(pipeline: Pipeline<dll::Memory>) -> PyResult<Self> {
        let memory = pipeline.timeline().memory();
        let symbols_by_address = memory
            .all_symbol_address()
            .into_iter()
            .map(|(key, value)| (value, key.to_owned()))
            .collect();

        Ok(Self {
            valid: Some(ValidPipeline {
                pipeline,
                symbols_by_address,
            }),
        })
    }

    fn invalidate(&mut self) -> Option<ValidPipeline> {
        self.valid.take()
    }

    fn get(&self) -> &ValidPipeline {
        self.valid.as_ref().expect("pipeline has been invalidated")
    }

    fn get_mut(&mut self) -> &mut ValidPipeline {
        self.valid.as_mut().expect("pipeline has been invalidated")
    }
}

#[pymethods]
impl PyPipeline {
    /// Load a new pipeline using the given DLL.
    ///
    /// To help ensure DLL safety and avoid memory leaks, this method also invalidates
    /// all existing pipelines that were created using this method.
    ///
    /// # Safety
    ///
    /// See `dll::Memory::load`. As long as the DLL is only loaded via this method,
    /// this method is safe.
    #[staticmethod]
    pub unsafe fn load(py: Python<'_>, dll_path: &str) -> PyResult<Py<Self>> {
        let mut valid_pipelines = VALID_PIPELINES.lock().unwrap();

        // Drop all known existing dll::Memory instances for safety
        for pipeline_py in valid_pipelines.drain(..) {
            pipeline_py.borrow_mut(py).invalidate();
        }

        let pipeline = load_dll_pipeline(dll_path, NUM_BACKUP_SLOTS)?;
        let pipeline_py = Py::new(py, PyPipeline::new(pipeline)?)?;

        valid_pipelines.push(pipeline_py.clone());

        Ok(pipeline_py)
    }

    /// Print the data layout to a string for debugging.
    pub fn dump_layout(&self) -> String {
        self.get()
            .pipeline
            .timeline()
            .memory()
            .data_layout()
            .to_string()
    }

    /// Read a variable.
    ///
    /// If the variable is a data variable, the value will be read from memory
    /// on the variable's frame.
    pub fn read(&self, py: Python<'_>, variable: &PyVariable) -> PyResult<PyObject> {
        let value = self.get().pipeline.read(&variable.variable)?;
        let py_object = value_to_py_object(py, &value)?;
        Ok(py_object)
    }

    /// Write a variable.
    ///
    /// If the variable is a data variable, the value will be truncated and written
    /// to memory on the variable's frame.
    pub fn write(
        &mut self,
        py: Python<'_>,
        variable: &PyVariable,
        value: PyObject,
    ) -> PyResult<()> {
        let value = py_object_to_value(py, &value)?;
        self.get_mut().pipeline.write(&variable.variable, &value)?;
        Ok(())
    }

    /// Reset a variable.
    pub fn reset(&mut self, variable: &PyVariable) -> PyResult<()> {
        self.get_mut().pipeline.reset(&variable.variable)?;
        Ok(())
    }

    /// Get the address for the given path.
    pub fn path_address(&self, frame: u32, path: &str) -> PyResult<PyAddress> {
        let state = self.get().pipeline.timeline().frame(frame)?;
        let address = state.address(path)?;
        Ok(PyAddress { address })
    }

    /// Read from the given path.
    pub fn path_read(&self, py: Python<'_>, frame: u32, path: &str) -> PyResult<PyObject> {
        let state = self.get().pipeline.timeline().frame(frame)?;
        let value = state.read(path)?;
        let py_object = value_to_py_object(py, &value)?;
        Ok(py_object)
    }

    /// Insert a new state at the given frame, shifting edits forward.
    pub fn insert_frame(&mut self, frame: u32) {
        self.get_mut().pipeline.insert_frame(frame);
    }

    /// Delete the state at the given frame, shifting edits backward.
    pub fn delete_frame(&mut self, frame: u32) {
        self.get_mut().pipeline.delete_frame(frame);
    }

    /// Set a hotspot, allowing for faster scrolling near the given frame.
    pub fn set_hotspot(&mut self, name: &str, frame: u32) {
        self.get_mut()
            .pipeline
            .timeline_mut()
            .set_hotspot(name, frame);
    }

    /// Perform housekeeping to improve scrolling near hotspots.
    pub fn balance_distribution(&mut self, max_run_time_seconds: f32) -> PyResult<()> {
        self.get_mut()
            .pipeline
            .timeline_mut()
            .balance_distribution(std::time::Duration::from_secs_f32(max_run_time_seconds))?;
        Ok(())
    }

    /// Return the label for the variable if it has one.
    pub fn label(&self, variable: &PyVariable) -> PyResult<Option<&str>> {
        let label = self
            .get()
            .pipeline
            .data_variables()
            .label(&variable.variable)?;
        Ok(label)
    }

    /// Return true if the variable has an integer data type.
    pub fn is_int(&self, variable: &PyVariable) -> PyResult<bool> {
        Ok(self
            .get()
            .pipeline
            .data_variables()
            .data_type(&variable.variable)?
            .is_int())
    }

    /// Return true if the variable has a float data type.
    pub fn is_float(&self, variable: &PyVariable) -> PyResult<bool> {
        Ok(self
            .get()
            .pipeline
            .data_variables()
            .data_type(&variable.variable)?
            .is_float())
    }

    /// Return true if the variable is a bit flag.
    pub fn is_bit_flag(&self, variable: &PyVariable) -> PyResult<bool> {
        Ok(self
            .get()
            .pipeline
            .data_variables()
            .flag(&variable.variable)?
            .is_some())
    }

    /// Get the variables
    fn variable_group(&self, group: &str) -> Vec<PyVariable> {
        self.get()
            .pipeline
            .data_variables()
            .group(group)
            .map(|variable| PyVariable { variable })
            .collect()
    }

    /// Translate an address into a raw pointer into the base slot.
    ///
    /// # Safety
    ///
    /// This should not be used to write to memory.
    /// This includes any functions that are called through it.
    ///
    /// The pipeline must stay live while this pointer is live.
    pub unsafe fn address_to_base_pointer(&self, address: &PyAddress) -> PyResult<usize> {
        let timeline = self.get().pipeline.timeline();
        let base_slot = timeline.base_slot(0)?;
        let pointer: *const u8 = timeline
            .memory()
            .address_to_base_pointer(base_slot.slot(), &address.address)?;
        Ok(pointer as usize)
    }

    /// Return the field offset for a path of the form `struct A.x`.
    pub fn field_offset(&self, path: &str) -> PyResult<usize> {
        let path = self.get().pipeline.timeline().memory().local_path(path)?;
        let offset = path.field_offset()?;
        Ok(offset)
    }

    /// Return a map from mario action values to human readable names.
    pub fn action_names(&self) -> HashMap<u32, String> {
        let data_layout = self.get().pipeline.timeline().memory().data_layout();
        data_layout
            .constants
            .iter()
            .filter(|(name, _)| {
                name.starts_with("ACT_")
                    && !name.starts_with("ACT_FLAG_")
                    && !name.starts_with("ACT_GROUP_")
                    && !name.starts_with("ACT_ID_")
            })
            .map(|(name, value)| {
                (
                    *value as u32,
                    name.strip_prefix("ACT_")
                        .unwrap()
                        .replace("_", " ")
                        .to_lowercase(),
                )
            })
            .collect()
    }

    /// Get the object behavior for an object, or None if the object is not active.
    pub fn object_behavior(&self, frame: u32, object: usize) -> PyResult<Option<PyObjectBehavior>> {
        let state = self.get().pipeline.timeline().frame(frame)?;

        match object_path(&state, ObjectSlot(object)) {
            Ok(object_path) => {
                let behavior = object_behavior(&state, &object_path)?;
                Ok(Some(PyObjectBehavior { behavior }))
            }
            Err(error) => {
                if let ErrorCause::SM64Error(SM64ErrorCause::InactiveObject { .. }) = &error.cause {
                    Ok(None)
                } else {
                    Err(error.into())
                }
            }
        }
    }

    /// Get a human readable name for the given object behavior, if possible.
    pub fn object_behavior_name(&self, behavior: &PyObjectBehavior) -> String {
        let address: dll::Address = behavior.behavior.0.into();
        let symbol = self.get().symbols_by_address.get(&address);

        if let Some(symbol) = symbol {
            symbol.strip_prefix("bhv").unwrap_or(symbol).to_owned()
        } else {
            format!("Object[{}]", address)
        }
    }
}

/// An abstract game variable.
#[pyclass(name = Variable, unsendable)]
#[derive(Debug, Display, Clone, PartialEq, Eq, Hash)]
pub struct PyVariable {
    variable: Variable,
}

#[pymethods]
impl PyVariable {
    /// Create a variable with the given name with no associated data.
    #[new]
    pub fn new(name: &str) -> Self {
        Self {
            variable: Variable::new(name),
        }
    }

    /// Get the name of the variable.
    #[getter]
    pub fn name(&self) -> &str {
        self.variable.name.as_ref()
    }

    /// Get the frame for the variable.
    #[getter]
    pub fn frame(&self) -> Option<u32> {
        self.variable.frame
    }

    /// Get the object slot for the variable.
    #[getter]
    pub fn object(&self) -> Option<usize> {
        self.variable.object.map(|slot| slot.0)
    }

    /// Get the object behavior for the variable.
    #[getter]
    pub fn object_behavior(&self) -> Option<PyObjectBehavior> {
        self.variable
            .object_behavior
            .clone()
            .map(|behavior| PyObjectBehavior { behavior })
    }

    /// Get the surface slot for the variable.
    #[getter]
    pub fn surface(&self) -> Option<usize> {
        self.variable.surface.map(|slot| slot.0)
    }

    /// Return a copy of the variable but associated with the given frame.
    pub fn with_frame(&self, frame: u32) -> Self {
        Self {
            variable: self.variable.with_frame(frame).into(),
        }
    }

    /// Return a copy of the variable but without an associated frame.
    pub fn without_frame(&self) -> Self {
        Self {
            variable: self.variable.without_frame().into(),
        }
    }

    /// Return a copy of the variable but associated to the given object slot.
    pub fn with_object(&self, object: usize) -> Self {
        Self {
            variable: self.variable.with_object(ObjectSlot(object)).into(),
        }
    }

    /// Return a copy of the variable but without an associated object slot.
    pub fn without_object(&self) -> Self {
        Self {
            variable: self.variable.without_object().into(),
        }
    }

    /// Return a copy of the variable but associated to the given object behavior.
    pub fn with_object_behavior(&self, behavior: &PyObjectBehavior) -> Self {
        Self {
            variable: self
                .variable
                .with_object_behavior(behavior.behavior.clone())
                .into(),
        }
    }

    /// Return a copy of the variable but without an associated object behavior.
    pub fn without_object_behavior(&self) -> Self {
        Self {
            variable: self.variable.without_object_behavior().into(),
        }
    }

    /// Return a copy of the variable but associated to the given surface slot.
    pub fn with_surface(&self, surface: usize) -> Self {
        Self {
            variable: self.variable.with_surface(SurfaceSlot(surface)).into(),
        }
    }

    /// Return a copy of the variable but without an associated surface slot.
    pub fn without_surface(&self) -> Self {
        Self {
            variable: self.variable.without_surface().into(),
        }
    }
}

#[pyproto]
impl PyObjectProtocol for PyVariable {
    fn __richcmp__(&self, other: PyVariable, op: CompareOp) -> PyResult<bool> {
        match op {
            CompareOp::Eq => Ok(self == &other),
            CompareOp::Ne => Ok(self != &other),
            _ => unimplemented!("{:?}", op),
        }
    }

    fn __hash__(&self) -> PyResult<isize> {
        let mut hasher = DefaultHasher::new();
        self.hash(&mut hasher);
        Ok(hasher.finish() as isize)
    }
}

/// An opaque representation of an object behavior.
#[pyclass(name = ObjectBehavior)]
#[derive(Debug)]
pub struct PyObjectBehavior {
    behavior: ObjectBehavior,
}

/// An opaque representation of a memory address.
#[pyclass(name = Address, unsendable)]
#[derive(Debug, Clone)]
pub struct PyAddress {
    address: dll::Address,
}

fn value_to_py_object(py: Python<'_>, value: &Value) -> PyResult<PyObject> {
    match value {
        Value::Int(n) => Ok(n.to_object(py)),
        Value::Float(r) => Ok(r.to_object(py)),
        Value::Address(address) => Ok(PyAddress {
            address: (*address).into(),
        }
        .into_py(py)),
        _ => Err(Error::from(SM64ErrorCause::ValueToPython {
            value: value.to_string(),
        })
        .into()),
    }
}

fn py_object_to_value(py: Python<'_>, value: &PyObject) -> PyResult<Value> {
    if let Ok(long_value) = value.cast_as::<PyLong>(py) {
        Ok(Value::Int(long_value.extract()?))
    } else if let Ok(float_value) = value.cast_as::<PyFloat>(py) {
        Ok(Value::Float(float_value.extract()?))
    } else if let Ok(address) = value.cast_as::<PyAny>(py)?.extract::<PyAddress>() {
        Ok(Value::Address(address.address.into()))
    } else {
        Err(Error::from(SM64ErrorCause::ValueFromPython {
            value: value.cast_as::<PyAny>(py)?.str()?.to_string()?.into(),
        })
        .into())
    }
}
