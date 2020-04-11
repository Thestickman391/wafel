from __future__ import annotations

from typing import *
from dataclasses import dataclass
import time

import wafel.imgui as ig
from wafel.variable import VariableAccessor, Variable
from wafel.variable_display import VariableDisplayer
from wafel.variable_format import Formatters, DecimalIntFormatter, VariableFormatter
from wafel.local_state import use_state_with, use_state
from wafel.frame_sheet import FrameSequence, FrameSheet, CellDragHandler
from wafel.util import *


@dataclass(frozen=True)
class EditRange:
  variable: Variable
  frames: range
  value: object

  def __contains__(self, variable: Variable) -> bool:
    frame = dcast(int, variable.args['frame'])
    return frame in self.frames and self.variable.at(frame=frame) == variable

  def intersects(self, other: EditRange) -> bool:
    if self.variable != other.variable:
      return False
    return self.frames.stop > other.frames.start and other.frames.stop > self.frames.start


@dataclass(frozen=True)
class OpAtom:
  target: Optional[EditRange]
  result: Optional[EditRange]

  def inverse(self) -> OpAtom:
    return OpAtom(self.result, self.target)

@dataclass(frozen=True)
class OpSequence:
  ops: Tuple[EditRangeOp, ...]

  def inverse(self) -> OpSequence:
    return OpSequence(tuple(op.inverse() for op in reversed(self.ops)))

EditRangeOp = Union[OpAtom, OpSequence]


class EditRanges:
  def __init__(self) -> None:
    self._all: Set[EditRange] = set()
    self._by_variable: Dict[Variable, EditRange] = {}

  def get(self, variable: Variable) -> Optional[EditRange]:
    return self._by_variable.get(variable)

  def by_variable(self, variable: Variable) -> Iterable[EditRange]:
    return (edit_range for edit_range in self._all if edit_range.variable == variable)

  def _remove(self, edit_range: EditRange) -> None:
    self._all.remove(edit_range)
    for variable, other in list(self._by_variable.items()):
      if other == edit_range:
        del self._by_variable[variable]

  def _add(self, edit_range: EditRange) -> None:
    self._all.add(edit_range)
    for frame in edit_range.frames:
      self._by_variable[edit_range.variable.at(frame=frame)] = edit_range

  def apply_mutate(self, op: EditRangeOp) -> None:
    if isinstance(op, OpAtom):
      if op.target is not None:
        assert op.target in self._all
        self._remove(op.target)
      if op.result is not None:
        for edit_range in self._all:
          assert not edit_range.intersects(op.result)
        self._add(op.result)
    elif isinstance(op, OpSequence):
      for child in op.ops:
        self.apply_mutate(child)
    else:
      raise NotImplementedError(op)


class Edits:
  def __init__(self) -> None:
    self.ranges = EditRanges()
    self._tentative_op: Optional[EditRangeOp] = None

  def apply(self, op: EditRangeOp) -> None:
    self.revert_tentative()
    self.ranges.apply_mutate(op)

  def get_range(self, variable: Variable) -> Optional[EditRange]:
    return self.ranges.get(variable)

  def get_edited_value(self, variable: Variable) -> Maybe[object]:
    edit_range = self.get_range(variable)
    if edit_range is None:
      return None
    else:
      return Just(edit_range.value)

  def set_edit(self, variable: Variable, value: object) -> None:
    self.revert_tentative()
    edit_range = self.get_range(variable)
    if edit_range is None:
      frame = dcast(int, variable.args['frame'])
      op = self.op_insert(variable.without('frame'), range(frame, frame + 1), value)
    else:
      op = self.op_set_value(edit_range, value)
    self.apply(op)

  def apply_tentative(self, op: EditRangeOp) -> None:
    assert self._tentative_op is None
    self.apply(op)
    self._tentative_op = op

  def revert_tentative(self) -> None:
    op = self._tentative_op
    if op is not None:
      self._tentative_op = None
      self.apply(op.inverse())

  def commit_tentative(self) -> None:
    self._tentative_op = None

  def op_no_op(self) -> EditRangeOp:
    return OpSequence(())

  def op_set_value(self, edit_range: EditRange, value: object) -> EditRangeOp:
    return OpAtom(edit_range, EditRange(edit_range.variable, edit_range.frames, value))

  def op_set_frames(self, edit_range: EditRange, to_frames: range) -> EditRangeOp:
    assert len(to_frames) > 0
    return OpAtom(edit_range, EditRange(edit_range.variable, to_frames, edit_range.value))

  def op_delete(self, edit_range: EditRange) -> EditRangeOp:
    return OpAtom(edit_range, None)

  def op_insert_range(self, edit_range: EditRange) -> EditRangeOp:
    assert len(edit_range.frames) > 0
    return OpAtom(None, edit_range)

  def op_insert(self, variable: Variable, frames: range, value: object) -> EditRangeOp:
    return self.op_insert_range(EditRange(variable, frames, value))

  # There is no op_sequence since op_grow depends on self.ranges, so sequencing
  # it isn't guaranteed to result in a valid operation

  def op_shrink(self, edit_range: EditRange, to_frames: range) -> EditRangeOp:
    if len(to_frames) > 0:
      assert to_frames.start >= edit_range.frames.start
      assert to_frames.stop <= edit_range.frames.stop
      return self.op_set_frames(edit_range, to_frames)
    else:
      return self.op_delete(edit_range)

  def op_resize(self, edit_range: EditRange, frames: range) -> EditRangeOp:
    assert self._tentative_op is None
    ops = []
    for other in self.ranges.by_variable(edit_range.variable):
      if other != edit_range:
        new_frames = other.frames
        if new_frames.stop in frames:
          new_frames = range(new_frames.start, frames.start)
        if new_frames.start in frames:
          new_frames = range(frames.stop, new_frames.stop)
        if new_frames != other.frames:
          ops.append(self.op_shrink(other, new_frames))
    if len(frames) > 0:
      ops.append(self.op_set_frames(edit_range, frames))
    else:
      ops.append(self.op_delete(edit_range))
    return OpSequence(tuple(ops))

  def op_include(self, edit_range: EditRange, frame: int) -> EditRangeOp:
    if frame < edit_range.frames.start:
      return self.op_resize(edit_range, range(frame, edit_range.frames.stop))
    if frame >= edit_range.frames.stop:
      return self.op_resize(edit_range, range(edit_range.frames.start, frame + 1))
    return self.op_no_op()

  def op_split_upward(self, edit_range: EditRange, gap: range) -> EditRangeOp:
    assert gap.stop - 1 in edit_range.frames
    ops = [self.op_shrink(edit_range, range(edit_range.frames.start, gap.start))]
    if gap.stop < edit_range.frames.stop:
      ops.append(self.op_insert(
        edit_range.variable,
        range(gap.stop, edit_range.frames.stop),
        edit_range.value,
      ))
    return OpSequence(tuple(ops))

  def op_split_downward(self, edit_range: EditRange, gap: range) -> EditRangeOp:
    assert gap.start in edit_range.frames
    ops = [self.op_shrink(edit_range, range(gap.stop, edit_range.frames.stop))]
    if edit_range.frames.start < gap.start:
      ops.append(self.op_insert(
        edit_range.variable,
        range(edit_range.frames.start, gap.start),
        edit_range.value,
      ))
    return OpSequence(tuple(ops))

  def op_insert_then_resize(
    self, variable: Variable, value: object, to_frames: range
  ) -> EditRangeOp:
    assert self._tentative_op is None
    assert self.ranges.get(variable) is None
    frame = variable.args['frame']
    edit_range = EditRange(variable.without('frame'), range(frame, frame + 1), value)
    return OpSequence((
      self.op_insert_range(edit_range),
      self.op_resize(edit_range, to_frames),
    ))


class Model(VariableAccessor, VariableDisplayer, Formatters, FrameSequence, CellDragHandler):
  def __init__(self) -> None:
    self._selected_frame = 0
    self._max_frame = 1000
    self._edits = Edits()
    for f in range(100):
      self._edits.set_edit(Variable('stick', frame=f), 10 + f)

  def get(self, variable: Variable) -> object:
    edit = self._edits.get_edited_value(variable)
    if edit is not None:
      return edit.value
    # elif variable.name == 'vel':
    #   if frame == -1:
    #     return 0
    #   else:
    #     prev_vel = dcast(int, self.get(Variable('vel', frame=frame - 1)))
    #     stick = dcast(int, self.get(Variable('stick', frame=frame)))
    #     return prev_vel + stick
    # elif variable.name == 'pos':
    #   if frame == -1:
    #     return 0
    #   else:
    #     prev_pos = dcast(int, self.get(Variable('pos', frame=frame - 1)))
    #     vel = dcast(int, self.get(Variable('vel', frame=frame)))
    #     return prev_pos + vel
    else:
      return 0

  def set(self, variable: Variable, value: object) -> None:
    self._edits.set_edit(variable, value)

  def drag(self, source: Variable, target_frame: int) -> None:
    self._edits.revert_tentative()

    source_frame = dcast(int, source.args['frame'])
    edit_range = self._edits.get_range(source)
    op: Optional[EditRangeOp] = None

    if edit_range is None:
      if target_frame > source_frame:
        op = self._edits.op_insert_then_resize(
          source, self.get(source), range(source_frame, target_frame + 1)
        )
      elif target_frame < source_frame:
        op = self._edits.op_insert_then_resize(
          source, self.get(source), range(target_frame, source_frame + 1)
        )
    elif edit_range.frames == range(source_frame, source_frame + 1):
      op = self._edits.op_include(edit_range, target_frame)
    elif edit_range.frames.start == source_frame:
      op = self._edits.op_resize(
        edit_range, range(target_frame, edit_range.frames.stop)
      )
    elif edit_range.frames.stop - 1 == source_frame:
      op = self._edits.op_resize(
        edit_range, range(edit_range.frames.start, target_frame + 1)
      )
    else:
      if target_frame > source_frame:
        op = self._edits.op_split_downward(
          edit_range, range(source_frame, target_frame)
        )
      elif target_frame < source_frame:
        op = self._edits.op_split_upward(
          edit_range, range(target_frame + 1, source_frame + 1)
        )

    if op is not None:
      self._edits.apply_tentative(op)

  def release(self) -> None:
    self._edits.commit_tentative()

  def highlight_range(self, variable: Variable) -> Optional[range]:
    edit_range = self._edits.get_range(variable)
    if edit_range is not None:
      return edit_range.frames
    else:
      return None

  def edited(self, variable: Variable) -> bool:
    return self._edits.get_range(variable) is not None

  def reset(self, variable: Variable) -> None:
    self._edits.revert_tentative()
    edit_range = self._edits.get_range(variable)
    if edit_range is not None:
      frame = dcast(int, variable.args['frame'])
      self._edits.apply(self._edits.op_split_upward(edit_range, range(frame, frame + 1)))

  def label(self, variable: Variable) -> str:
    return variable.name

  def column_header(self, variable: Variable) -> str:
    return variable.name

  def __getitem__(self, variable: Variable) -> VariableFormatter:
    return DecimalIntFormatter()

  @property
  def selected_frame(self) -> int:
    return self._selected_frame

  def set_selected_frame(self, frame: int) -> None:
    self._selected_frame = frame

  @property
  def max_frame(self) -> int:
    return self._max_frame

  def extend_to_frame(self, frame: int) -> None:
    self._max_frame = max(self._max_frame, frame)

  def insert_frame(self, frame: int) -> None:
    pass

  def delete_frame(self, frame: int) -> None:
    pass

  def set_hotspot(self, name: str, frame: int) -> None:
    pass


def test_frame_sheet(id: str) -> None:
  ig.push_id(id)
  model = use_state_with('model', lambda: Model()).value

  def make_sheet() -> FrameSheet:
    sheet = FrameSheet(*([model] * 5))
    sheet.append_variable(Variable('stick'))
    sheet.append_variable(Variable('vel'))
    sheet.append_variable(Variable('pos'))
    for i in range(20):
      sheet.append_variable(Variable('junk ' + str(i)))
    return sheet
  sheet = use_state_with('sheet', make_sheet).value

  ig.set_next_window_content_size(sheet.get_content_width(), 0)
  ig.begin_child(
    'frame-sheet',
    height = ig.get_window_height() * 0.95,
    flags = ig.WINDOW_HORIZONTAL_SCROLLING_BAR,
  )
  sheet.render()
  ig.end_child()

  last_fps_time = use_state_with('last-fps-time', lambda: time.time())
  frame_count = use_state('frame-count', 0)
  fps = use_state('fps', 0.0)

  frame_count.value += 1
  if time.time() > last_fps_time.value + 1:
    fps.value = frame_count.value / (time.time() - last_fps_time.value)
    last_fps_time.value = time.time()
    frame_count.value = 0
    print(f'mspf: {int(1000 / fps.value * 10) / 10} ({int(fps.value)} fps)')

  ig.pop_id()