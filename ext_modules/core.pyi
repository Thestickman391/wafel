from __future__ import annotations

from typing import *


class Pipeline:
  @staticmethod
  def load(dll_path: str) -> Pipeline: ...
  def dump_layout(self) -> str: ...

  def read(self, variable: Variable) -> object: ...
  def write(self, variable: Variable, value: object) -> None: ...
  def reset(self, variable: Variable) -> None: ...

  def path_address(self, frame: int, path: str) -> Address: ...
  def path_read(self, frame: int, path: str) -> object: ...

  def insert_frame(self, frame: int) -> None: ...
  def delete_frame(self, frame: int) -> None: ...

  def set_hotspot(self, name: str, frame: int) -> None: ...
  def balance_distribution(self, max_run_time_seconds: float) -> None: ...
  def cached_frames(self) -> List[int]: ...

  def label(self, variable: Variable) -> Optional[str]: ...
  def is_int(self, variable: Variable) -> bool: ...
  def is_float(self, variable: Variable) -> bool: ...
  def is_bit_flag(self, variable: Variable) -> bool: ...

  def variable_group(self, group: str) -> List[Variable]: ...
  def address_to_base_pointer(self, address: Address) -> int: ...
  def field_offset(self, path: str) -> int: ...
  def action_names(self) -> Dict[int, str]: ...
  def object_behavior(self, frame: int, object: int) -> Optional[ObjectBehavior]: ...
  def object_behavior_name(self, behavior: ObjectBehavior) -> str: ...


class Variable:
  def __init__(self, name: str) -> None: ...
  @property
  def name(self) -> str: ...
  @property
  def frame(self) -> Optional[int]: ...
  @property
  def object(self) -> Optional[int]: ...
  @property
  def object_behavior(self) -> Optional[ObjectBehavior]: ...
  @property
  def surface(self) -> Optional[int]: ...
  def with_frame(self, frame: int) -> Variable: ...
  def without_frame(self) -> Variable: ...
  def with_object(self, object: int) -> Variable: ...
  def without_object(self) -> Variable: ...
  def with_object_behavior(self, behavior: ObjectBehavior) -> Variable: ...
  def without_object_behavior(self) -> Variable: ...
  def with_surface(self, surface: int) -> Variable: ...
  def without_surface(self) -> Variable: ...


class ObjectBehavior:
  pass


class Address:
  pass
