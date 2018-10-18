# Copyright 2017 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import collections
import functools
from multiprocessing.pool import ThreadPool
import sys
import weakref

from absl.testing import parameterized
import numpy

from tensorflow.core.protobuf import config_pb2
from tensorflow.core.protobuf import rewriter_config_pb2
from tensorflow.python import keras
from tensorflow.python.eager import backprop
from tensorflow.python.eager import context
from tensorflow.python.eager import def_function
from tensorflow.python.eager import function
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import errors
from tensorflow.python.framework import function as tf_function
from tensorflow.python.framework import ops
from tensorflow.python.framework import random_seed
from tensorflow.python.framework import tensor_shape
from tensorflow.python.framework import tensor_spec
from tensorflow.python.framework import test_ops
from tensorflow.python.framework import test_util
from tensorflow.python.keras.engine import training as keras_training
from tensorflow.python.layers import convolutional
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import clip_ops
from tensorflow.python.ops import control_flow_ops
from tensorflow.python.ops import gradients_impl
from tensorflow.python.ops import init_ops
from tensorflow.python.ops import list_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import random_ops
from tensorflow.python.ops import resource_variable_ops
from tensorflow.python.ops import variable_scope
from tensorflow.python.ops import variables
from tensorflow.python.platform import test
from tensorflow.python.training import adam
from tensorflow.python.training import momentum
from tensorflow.python.training import training_ops
from tensorflow.python.util import compat
from tensorflow.python.util import nest
from tensorflow.python.util import tf_inspect


class MiniModel(keras_training.Model):
  """Minimal model for mnist.

  Useful for testing and debugging on slow TPU simulators.
  """

  def __init__(self):
    super(MiniModel, self).__init__(name='')
    self.fc = keras.layers.Dense(1, name='fc', kernel_initializer='ones',
                                 bias_initializer='ones')

  def call(self, inputs, training=True):
    return self.fc(inputs)


class DefunnedMiniModel(MiniModel):

  @function.defun
  def call(self, inputs, training=True):
    return super(DefunnedMiniModel, self).call(inputs, training=training)


@test_util.with_c_shapes
class FunctionTest(test.TestCase, parameterized.TestCase):

  def testBasic(self):
    matmul = function.defun(math_ops.matmul)
    t = constant_op.constant([[1.0, 2.0], [3.0, 4.0]])
    sq = matmul(t, t, transpose_a=True)
    sq2 = matmul(sq, t, transpose_a=True)
    self.assertAllEqual(sq.numpy().reshape(-1), [10, 14, 14, 20])
    self.assertAllEqual(sq2.numpy().reshape(-1), [52, 76, 74, 108])

  def testWastedAdd(self):

    @function.defun()
    def add(x, y):
      _ = x * y
      return x + y

    # The default config allows everything.
    rewrites = rewriter_config_pb2.RewriterConfig()

    with context.rewriter_config(rewrites):
      t = constant_op.constant(1.0)
      self.assertAllEqual(add(t, t).numpy(), 2.0)

  def testBasicGraphMode(self):
    matmul = function.defun(math_ops.matmul)

    @function.defun
    def sq(a):
      return matmul(a, a)

    t = constant_op.constant([[1.0, 2.0], [3.0, 4.0]])
    out = sq(t)
    self.assertAllEqual(out, math_ops.matmul(t, t).numpy())

  def testNestedInputsGraphMode(self):
    matmul = function.defun(math_ops.matmul)

    pair = collections.namedtuple('pair', ['a', 'b'])

    @function.defun
    def a_times_b(inputs):
      return matmul(inputs.a['a'], inputs.b['b'])

    t = constant_op.constant([[1.0, 2.0], [3.0, 4.0]])

    out = a_times_b(pair({'a': t}, {'b': t}))
    self.assertAllEqual(out, math_ops.matmul(t, t).numpy())

  def testGraphModeWithGradients(self):
    v = resource_variable_ops.ResourceVariable(1.0, name='v')

    @function.defun
    def step():
      def inner():
        return v * v

      return backprop.implicit_grad(inner)()[0][0]

    self.assertAllEqual(step(), 2.0)

  def testGraphGradientVariable(self):
    with ops.Graph().as_default(), self.cached_session():
      v = resource_variable_ops.ResourceVariable(1.0)

      @function.defun
      def f():
        return 2.0 * v

      node = f()
      grads, = gradients_impl.gradients(node, v)
      v.initializer.run()
      self.assertAllEqual(grads.eval(), 2.0)
      self.assertEqual(grads.shape, v.shape)

  def testGraphEagerIsolation(self):

    @function.defun
    def f():
      self.v = resource_variable_ops.ResourceVariable(1.0)
      return self.v.read_value()

    self.assertAllEqual(f(), 1.0)

    with ops.Graph().as_default():
      self.assertEqual(f().shape, ())

  def testBasicGraphFunction(self):
    matmul = function.defun(math_ops.matmul)

    @function.defun
    def sq(a):
      return matmul(a, a)

    t = constant_op.constant([[1.0, 2.0], [3.0, 4.0]])

    sq_op = sq.get_concrete_function(t)
    self.assertEqual(sq_op.output_shapes, tensor_shape.TensorShape([2, 2]))
    out = sq_op(t)
    self.assertAllEqual(out, math_ops.matmul(t, t).numpy())

  def testInputSpecGraphFunction(self):
    matmul = function.defun(math_ops.matmul)

    @function.defun
    def sq(a):
      return matmul(a, a)

    sq_op = sq.get_concrete_function(
        tensor_spec.TensorSpec((None, None), dtypes.float32))
    self.assertEqual([None, None], sq_op.output_shapes.as_list())

    t1 = constant_op.constant([[1.0, 2.0], [3.0, 4.0]])
    out1 = sq_op(t1)
    self.assertAllEqual(out1, math_ops.matmul(t1, t1).numpy())

    t2 = constant_op.constant([[1.0, 2.0], [3.0, 4.0]])
    out2 = sq_op(t2)
    self.assertAllEqual(out2, math_ops.matmul(t2, t2).numpy())

  def testNestedInputSpecGraphFunction(self):
    matmul = function.defun(math_ops.matmul)

    @function.defun
    def sq(mats):
      ((a, b),) = mats
      return matmul(a, b)

    sq_op = sq.get_concrete_function(
        [(tensor_spec.TensorSpec((None, None), dtypes.float32),
          tensor_spec.TensorSpec((None, None), dtypes.float32))])
    self.assertEqual([None, None], sq_op.output_shapes.as_list())

    t1 = constant_op.constant([[1.0, 2.0], [3.0, 4.0]])
    t2 = constant_op.constant([[1.4, 2.4], [3.4, 4.4]])
    out = sq_op(t1, t2)  # Flattened structure for inputs to the graph function
    self.assertAllEqual(out, math_ops.matmul(t1, t2).numpy())

  def testExecutingStatelessDefunConcurrently(self):

    @function.defun
    def stateless(x):
      return math_ops.multiply(2.0, x)

    pool = ThreadPool()
    inputs = [constant_op.constant(1.0 * x) for x in range(100)]
    outputs = [float(out) for out in pool.map(stateless, inputs)]
    expected = [float(2.0 * x) for x in inputs]
    self.assertSequenceEqual(outputs, expected)

  def testExecutingManyStatelessDefunsConcurrently(self):

    @function.defun
    def stateless(x):
      del x
      return math_ops.multiply(2.0, 2.0)

    pool = ThreadPool()
    # `pool.map` below instantiates 100 functions, one for each object.
    outputs = [
        float(out)
        for out in pool.map(stateless, [object() for _ in range(100)])
    ]
    expected = [4.0] * 100
    self.assertSequenceEqual(outputs, expected)

  def testExecutingStatefulDefunConcurrently(self):

    v = resource_variable_ops.ResourceVariable(1.0)

    @function.defun
    def stateful(x):
      v.assign(x)

    pool = ThreadPool()
    inputs = [constant_op.constant(0.0)] * 100
    pool.map(stateful, inputs)
    self.assertEqual(float(v.read_value()), 0.0)

  def testExecutingManyStatefulDefunsConcurrently(self):

    v = resource_variable_ops.ResourceVariable(1.0)

    @function.defun
    def stateful(x):
      del x
      return v.assign(0.0)

    pool = ThreadPool()
    # `pool.map` below instantiates 100 functions, one for each object.
    pool.map(stateful, [object() for _ in range(100)])
    self.assertEqual(float(v.read_value()), 0.0)

  def disabled_testRandomSeed(self):

    @function.defun
    def f():
      return random_ops.random_normal(())

    random_seed.set_random_seed(1)
    x = f()
    self.assertNotEqual(x, f())
    random_seed.set_random_seed(1)
    self.assertAllEqual(f(), x)

  def testSymGradGatherNd(self):
    with ops.Graph().as_default(), self.cached_session() as sess:

      @function.defun
      def f(x):
        return array_ops.gather_nd(x, [[0]])

      c = constant_op.constant([[2.]])
      f_c = f(c)
      g, = gradients_impl.gradients(f_c, c)
      self.assertAllEqual(sess.run(g).values, [[1.0]])

  def testNoSymGradNestedDefun(self):

    @function.defun
    def outer():

      @function.defun
      def f(x):
        return array_ops.gather_nd(x, [[0]])

      c = constant_op.constant([[2.]])
      f_c = f(c)
      g, = gradients_impl.gradients(f_c, c)
      self.assertTrue(isinstance(g, ops.IndexedSlices))

    outer()

  def testNestedInputsGraphFunction(self):
    matmul = function.defun(math_ops.matmul)

    pair = collections.namedtuple('pair', ['a', 'b'])

    @function.defun
    def a_times_b(inputs):
      return matmul(inputs.a['a'], inputs.b['b'])

    t = constant_op.constant([[1.0, 2.0], [3.0, 4.0]])
    inputs = pair({'a': t}, {'b': t})
    sq_op = a_times_b.get_concrete_function(inputs)
    self.assertEqual(sq_op.output_shapes, tensor_shape.TensorShape([2, 2]))
    out = sq_op(inputs)
    self.assertAllEqual(out, math_ops.matmul(t, t).numpy())

  def testNestedOutputGraphFunction(self):
    matmul = function.defun(math_ops.matmul)

    @function.defun
    def sq(a):
      return (matmul(a, a), {'b': constant_op.constant(1.0)})

    t = constant_op.constant([[1.0, 2.0], [3.0, 4.0]])

    sq_op = sq.get_concrete_function(t)
    self.assertEqual(sq_op.output_shapes,
                     (tensor_shape.TensorShape([2, 2]),
                      {'b': tensor_shape.TensorShape([])}))
    self.assertEqual(sq_op.output_dtypes,
                     (dtypes.float32, {'b': dtypes.float32}))
    (a, b) = sq_op(t)
    self.assertAllEqual(a, math_ops.matmul(t, t).numpy())
    self.assertAllEqual(b['b'].numpy(), 1.0)

  def testGraphFunctionWithGradients(self):
    v = resource_variable_ops.ResourceVariable(1.0, name='v')

    @function.defun
    def step():
      def inner():
        return v * v

      return backprop.implicit_grad(inner)()[0][0]

    step_op = step.get_concrete_function()
    self.assertEqual(step_op.output_dtypes, dtypes.float32)
    self.assertEqual(step_op.output_shapes, tensor_shape.TensorShape([]))
    self.assertAllEqual(step_op(), 2.0)

  def testGraphFunctionNoneOutput(self):
    @function.defun
    def fn(unused_a, unused_b):
      return None

    x = constant_op.constant(1)
    fn_op = fn.get_concrete_function(x, x)
    self.assertEqual(fn_op.output_dtypes, None)
    self.assertEqual(fn_op.output_shapes, None)
    self.assertAllEqual(fn_op(x, x), None)

  @test_util.run_in_graph_and_eager_modes()
  def testDefunCondGradient(self):

    @function.defun
    def f(x):
      return control_flow_ops.cond(x > 0.5, lambda: 2 * x, lambda: 3 * x)

    with backprop.GradientTape() as t:
      x = constant_op.constant(1.0)
      t.watch(x)
      y = f(x)
    self.assertAllEqual(self.evaluate(t.gradient(y, x)), 2.0)

  @test_util.run_in_graph_and_eager_modes()
  def testGraphLoopGradient(self):

    @function.defun
    def f(x):
      return control_flow_ops.while_loop(lambda _, i: i < 2,
                                         lambda x, i: (2*x, i + 1),
                                         [x, 0])[0]

    with backprop.GradientTape() as t:
      x = constant_op.constant(1.0)
      t.watch(x)
      y = f(x)
    self.assertAllEqual(self.evaluate(t.gradient(y, x)), 4.0)

  def testDefunNumpyArraysConvertedToTensors(self):

    def f(x):
      self.assertIsInstance(x, ops.Tensor)
      return x

    x = random_ops.random_uniform([2, 2]).numpy()
    defined = function.defun(f)
    defined(x)
    self.assertEqual(len(defined._function_cache), 1)

    x = random_ops.random_uniform([2, 2]).numpy()
    defined(x)
    # A NumPy array with different values but the same shape and dtype
    # shouldn't trigger another function definition.
    self.assertEqual(len(defined._function_cache), 1)

    # Test that the numpy array is properly an argument to the graph function.
    self.assertEqual(1., defined(numpy.ones([])).numpy())
    self.assertEqual(0., defined(numpy.zeros([])).numpy())
    self.assertEqual(1., defined(array_ops.ones([])).numpy())
    self.assertEqual(0., defined(array_ops.zeros([])).numpy())

  def testDefunCapturedInt32(self):
    x = constant_op.constant(1, dtype=dtypes.int32)

    @function.defun
    def add_int32s():
      return x + x

    self.assertEqual(2, int(add_int32s()))

  def testDefunReadVariable(self):
    v = resource_variable_ops.ResourceVariable(1.0)

    @function.defun
    def f():
      return v.read_value()

    self.assertEqual(1.0, float(f()))

  def testDefunAssignAddVariable(self):
    v = resource_variable_ops.ResourceVariable(1.0)
    x = constant_op.constant(2.0)

    @function.defun
    def test_assign_add():
      v.assign_add(x)
      return v.read_value()

    self.assertEqual(3.0, float(test_assign_add()))

  @test_util.run_in_graph_and_eager_modes
  def testTensorInitializationInFunctionRaisesError(self):
    error_msg = ('Tensor-typed variable initializers must either be '
                 'wrapped in an init_scope or callable.*')

    @function.defun
    def tensor_init():
      with self.assertRaisesRegexp(ValueError, error_msg):
        resource_variable_ops.ResourceVariable(constant_op.constant(2.0))

    tensor_init()

  @test_util.run_in_graph_and_eager_modes
  def testCallableTensorInitializationInFunction(self):

    @function.defun
    def tensor_init():
      self.v = resource_variable_ops.ResourceVariable(
          lambda: constant_op.constant(2.0))
      return self.v.read_value()

    value = tensor_init()
    if not context.executing_eagerly():
      self.evaluate(variables.global_variables_initializer())
    self.assertEqual(self.evaluate(value), 2.0)

  @test_util.run_in_graph_and_eager_modes
  def testInitScopeTensorInitializationInFunction(self):

    @function.defun
    def tensor_init():
      with ops.init_scope():
        const = constant_op.constant(2.0)
      self.v = resource_variable_ops.ResourceVariable(const)
      return self.v.read_value()

    value = tensor_init()
    if not context.executing_eagerly():
      self.evaluate(variables.global_variables_initializer())
    self.assertEqual(self.evaluate(value), 2.0)

  def testDefunShapeInferenceWithCapturedResourceVariable(self):
    v = resource_variable_ops.ResourceVariable([[1, 2], [3, 4]])

    def f():
      x = constant_op.constant([[1, 2], [3, 4]])
      out = math_ops.matmul(v, x)
      self.assertEqual(out.shape, tensor_shape.TensorShape([2, 2]))
      # We do not return v directly since the tensor conversion function of
      # ResourceVariable returns the read value and not the resource itself.
      return v._handle

    compiled = function.defun(f)
    var_handle = compiled()
    self.assertEqual(var_handle.dtype, dtypes.resource)
    self.assertEqual(var_handle.shape, tensor_shape.scalar())
    var_t = resource_variable_ops.read_variable_op(var_handle, dtype=v.dtype)
    self.assertEqual(var_t.shape, tensor_shape.TensorShape([2, 2]))

  def testVariableInLoopInFunction(self):

    @function.defun
    def test_function():

      def loop_test(_):
        return False

      def loop_body(_):
        return variable_scope.get_variable('a', shape=())

      return control_flow_ops.while_loop(loop_test, loop_body, [0.0])

    self.assertEqual(test_function().shape, [])

  def testDefunShapeInferenceWithCapturedResourceVariableInGraphMode(self):
    with context.graph_mode():
      v = resource_variable_ops.ResourceVariable([[1, 2], [3, 4]])

      def f():
        x = constant_op.constant([[1, 2], [3, 4]])
        out = math_ops.matmul(v, x)
        self.assertEqual(out.shape, tensor_shape.TensorShape([2, 2]))
        # We do not return v directly since the tensor conversion function of
        # ResourceVariable returns the read value and not the resource itself.
        return v._handle

      compiled = function.defun(f)
      var_handle = compiled()
      self.assertEqual(var_handle.dtype, dtypes.resource)
      self.assertEqual(var_handle.shape, tensor_shape.scalar())
      var_t = resource_variable_ops.read_variable_op(var_handle, dtype=v.dtype)
      self.assertEqual(var_t.shape, tensor_shape.TensorShape([2, 2]))

  def testDefunShapeInferenceWithCapturedVariableInGraphMode(self):
    with context.graph_mode():
      v = variables.Variable([[1, 2], [3, 4]])

      def f():
        x = constant_op.constant([[1, 2], [3, 4]])
        out = math_ops.matmul(v, x)
        self.assertEqual(out.shape, tensor_shape.TensorShape([2, 2]))

      # Check that shape inference works while creating the defun
      compiled = function.defun(f)
      compiled()

  def testDefunShapeInferenceWithCapturedTensorListInGraphMode(self):
    with context.graph_mode():
      tensor_list = list_ops.empty_tensor_list(
          element_dtype=dtypes.float32,
          element_shape=ops.convert_to_tensor([], dtype=dtypes.int32))
      tensor_list = list_ops.tensor_list_push_back(tensor_list,
                                                   constant_op.constant(1.0))
      tensor_list = list_ops.tensor_list_push_back(tensor_list,
                                                   constant_op.constant(2.0))

      def f():
        tl, value = list_ops.tensor_list_pop_back(
            tensor_list, element_dtype=dtypes.float32)
        self.assertEqual(value.shape, tensor_shape.scalar())
        return tl

      compiled = function.defun(f)
      output_tensor_list = compiled()
      _, value = list_ops.tensor_list_pop_back(
          output_tensor_list, element_dtype=dtypes.float32)
      self.assertEqual(value.shape, tensor_shape.scalar())

  @test_util.run_in_graph_and_eager_modes
  def testDefunForcesResourceVariables(self):

    def variable_creator():
      self.v = variables.Variable(0.0)
      return self.v.read_value()

    self.v = None
    defined = function.defun(variable_creator)
    defined()  # Create the variable.
    self.assertIsInstance(
        self.v, resource_variable_ops.ResourceVariable)

  def testDefunDifferentiable(self):
    v = resource_variable_ops.ResourceVariable(1.0)

    @function.defun
    def f():
      return v * v

    self.assertAllEqual(backprop.implicit_grad(f)()[0][0], 2.0)

  def testDefunCanBeDifferentiatedTwice(self):
    v = resource_variable_ops.ResourceVariable(1.0)

    @function.defun
    def f():
      return v * v

    self.assertAllEqual(backprop.implicit_grad(f)()[0][0], 2.0)
    # Ensure that v is watched again.
    self.assertAllEqual(backprop.implicit_grad(f)()[0][0], 2.0)

  def testRunMetadata(self):

    @function.defun
    def f(x):
      return x * x

    with ops.device('cpu:0'):
      f(constant_op.constant(1.0))  # pre-build the defun
      context.enable_run_metadata()
      f(constant_op.constant(1.0))
    run_metadata = context.export_run_metadata()
    context.disable_run_metadata()
    step_stats = run_metadata.step_stats
    self.assertGreater(len(step_stats.dev_stats), 0)
    cpu_stats = step_stats.dev_stats[0]
    self.assertEqual('/job:localhost/replica:0/task:0/device:CPU:0',
                     cpu_stats.device)
    # Testing for at least 2 because the function call should generate at most
    # one entry in the step_stats; the ops inside function can generate
    # arbitrarily many (placeholders, return identities, etc, might be included
    # or not in the future, so shouldn't be tested for exactly.
    self.assertGreaterEqual(len(cpu_stats.node_stats), 2)

  def testGraphModeCaptureVariable(self):
    with context.graph_mode(), self.cached_session() as sess:

      class HasAVar(object):

        def __init__(self):
          self.v = resource_variable_ops.ResourceVariable(1.0)

        def call(self):
          return self.v * 2

      o = HasAVar()
      variables.global_variables_initializer().run()
      call = function.defun(o.call)
      op = call()
      self.assertAllEqual(sess.run(op), 2.0)

  def testSymbolicGradientVariableNoneNotZerosLike(self):
    with ops.Graph().as_default():
      v = resource_variable_ops.ResourceVariable(1.0)

      @function.defun
      def f(x, v):
        v.read_value()
        return x * x

      x = constant_op.constant(1.0)
      l = f(x, v)
      _, dv = gradients_impl.gradients(l, [x, v])
      with self.cached_session():
        v.initializer.run()
        self.assertEqual(dv, None)

  def testGraphModeManyFunctions(self):
    with context.graph_mode(), self.cached_session():

      @function.defun
      def f(x):
        return x * x

      @function.defun
      def g(x):
        return f(x) + 1

      self.assertAllEqual(g(constant_op.constant(2.0)).eval(), 5.0)

  def testDict(self):

    @function.defun
    def f(x):
      return {'name': x + 1}

    self.assertAllEqual(f(constant_op.constant(1.0))['name'], 2.0)

  def testTensorConversionWithDefun(self):

    @function.defun
    def f(x):
      return math_ops.add(x, constant_op.constant(3))

    self.assertAllEqual(5, f(constant_op.constant(2)))

  def testTensorConversionCall(self):

    @function.defun
    def f(x):
      return math_ops.add(x, constant_op.constant(3))

    @function.defun
    def g(x):
      return f(f(x))

    self.assertAllEqual(8, g(constant_op.constant(2)))

  def testDefunCallBackprop(self):

    @function.defun
    def f(x):
      return math_ops.add(x, x)

    @function.defun
    def g(x):
      return backprop.gradients_function(f, [0])(x)[0]

    self.assertAllEqual(2, g(constant_op.constant(2.)))

  def testGraphModeEagerGradError(self):
    with context.graph_mode():
      def f():
        x = variable_scope.get_variable(
            'v', initializer=constant_op.constant(1.0))
        return x * constant_op.constant(2.0)

      with self.assertRaisesRegexp(ValueError,
                                   'No trainable variables were accessed'):
        backprop.implicit_val_and_grad(f)()

  def testDefunCallBackpropUsingSameObjectForMultipleArguments(self):

    @function.defun
    def g(x):
      return backprop.gradients_function(math_ops.multiply, [0, 1])(x, x)

    def np_g(x):
      return [d.numpy() for d in g(x)]

    x = constant_op.constant(1.)
    self.assertAllEqual([1., 1.], np_g(x))
    self.assertAllEqual([1., 1.], np_g(1.))

  def testCallShape(self):

    @function.defun
    def f(x):
      return x + 1

    @function.defun
    def g(x):
      x = f(x)
      self.assertEqual(x.shape.as_list(), [])
      return None

    g(constant_op.constant(1.0))

  def testNestedDefunWithNoOutputAndTapedInput(self):
    three = resource_variable_ops.ResourceVariable(3.0, name='v')

    @function.defun
    def f(x):
      # This function intentionally takes a taped variable as input,
      # but does not return any values
      math_ops.add(x, three)

    @function.defun
    def g(x):
      y = math_ops.add(x, three)
      f(y)

    g(three)

  def testGradientTensorConversionWithDefun(self):
    three = resource_variable_ops.ResourceVariable(3.0, name='v')

    @function.defun
    def f(x):
      return math_ops.add(x, three)

    def g(x):
      return f(x)

    g = backprop.implicit_grad(g)(constant_op.constant(1.0))[0][0]
    self.assertAllEqual(g, 1.0)

  def testGradient(self):
    matmul = function.defun(math_ops.matmul)

    def sq(x):
      return matmul(x, x, transpose_a=True)

    t = constant_op.constant([[1.0, 2.0], [3.0, 4.0]])
    grad_t, = backprop.gradients_function(sq, [0])(t)
    self.assertAllEqual(grad_t, [[6, 6], [14, 14]])

  def testGradientInFunction(self):

    @function.defun
    def f(x):
      return backprop.gradients_function(lambda y: y * y, [0])(x)[0]

    self.assertAllEqual(f(constant_op.constant(1.0)), 2.0)

  def testGatherResourceWithDefun(self):
    with ops.device('cpu:0'):
      v = resource_variable_ops.ResourceVariable([0.0, 1.0, 2.0])

    def sum_gather():
      return math_ops.reduce_sum(array_ops.gather(v, [1, 2]))

    defined = function.defun(sum_gather)
    self.assertAllEqual(sum_gather(), defined())

  def testGradientOfGatherWithDefun(self):
    v = resource_variable_ops.ResourceVariable([0.0, 1.0, 2.0])

    def sum_gather():
      return math_ops.reduce_sum(array_ops.gather(v, [1, 2]))

    grad_fn = backprop.implicit_grad(sum_gather)
    gradient = grad_fn()
    defun_grad_fn = backprop.implicit_grad(function.defun(sum_gather))
    defun_gradient = defun_grad_fn()
    self.assertEqual(len(gradient), len(defun_gradient))

    gradient = gradient[0][0]
    defun_gradient = defun_gradient[0][0]
    self.assertAllEqual(gradient.values, defun_gradient.values)
    self.assertAllEqual(gradient.indices, defun_gradient.indices)
    self.assertAllEqual(gradient.dense_shape, defun_gradient.dense_shape)

  def testReturningIndexedSlicesWithDefun(self):

    def validate(indexed_slice):
      @function.defun
      def f():
        return indexed_slice

      output = f()
      self.assertTrue(isinstance(output, ops.IndexedSlices))
      self.assertAllEqual(indexed_slice.values, output.values)
      self.assertAllEqual(indexed_slice.indices, output.indices)
      self.assertAllEqual(indexed_slice.dense_shape, output.dense_shape)

      self.assertEqual(
          f.get_concrete_function().output_shapes,
          indexed_slice.values.shape)

    arg = ops.IndexedSlices(
        values=constant_op.constant([1, 2]),
        indices=constant_op.constant([0, 1]),
        dense_shape=constant_op.constant([2]))
    validate(arg)

    arg = ops.IndexedSlices(
        values=constant_op.constant([1, 2]),
        indices=constant_op.constant([0, 1]),
        dense_shape=None)
    validate(arg)

  def testIndexedSliceAsArgumentWithDefun(self):

    @function.defun
    def f(indexed_slice):
      return indexed_slice

    def validate(arg):
      output = f(arg)
      self.assertTrue(isinstance(output, ops.IndexedSlices))
      self.assertAllEqual(arg.values, output.values)
      self.assertAllEqual(arg.indices, output.indices)
      self.assertAllEqual(arg.dense_shape, output.dense_shape)

    indexed_slice = ops.IndexedSlices(
        values=constant_op.constant([1]),
        indices=constant_op.constant([0]),
        dense_shape=constant_op.constant([1]))
    validate(indexed_slice)

    # Test that `f` works even when `dense_shape` is None.
    indexed_slice = ops.IndexedSlices(
        values=constant_op.constant([1]),
        indices=constant_op.constant([0]),
        dense_shape=None)
    validate(indexed_slice)

  def testFunctionOnDevice(self):
    if not context.context().num_gpus():
      self.skipTest('No GPUs found')

    x = constant_op.constant([1.]).gpu()
    f = function.defun(math_ops.add)
    y = f(x, x).cpu()
    self.assertAllEqual(y, [2.])

  @test_util.run_in_graph_and_eager_modes
  def testFunctionWithResourcesOnDifferentDevices(self):
    if not context.context().num_gpus():
      self.skipTest('No GPUs found.')

    with ops.device('/cpu:0'):
      v_cpu = resource_variable_ops.ResourceVariable([0.0, 1.0, 2.0])

    with ops.device('/gpu:0'):
      v_gpu = resource_variable_ops.ResourceVariable([0.0, 1.0, 2.0])

    def sum_gather():
      cpu_result = math_ops.reduce_sum(array_ops.gather(v_cpu, [1, 2]))
      gpu_result = math_ops.reduce_sum(array_ops.gather(v_gpu, [1, 2]))
      return cpu_result, gpu_result

    defined = function.defun(sum_gather)
    if not context.executing_eagerly():
      self.evaluate(variables.global_variables_initializer())
    expected = self.evaluate(sum_gather())
    self.assertAllEqual(expected, self.evaluate(defined()))

  @test_util.run_in_graph_and_eager_modes
  def testOpInFunctionWithConflictingResourceInputs(self):
    if not context.context().num_gpus():
      self.skipTest('No GPUs found.')

    with ops.device('/cpu:0'):
      v_cpu = resource_variable_ops.ResourceVariable(
          [0.0, 1.0, 2.0], name='cpu')
      v_also_cpu = resource_variable_ops.ResourceVariable(
          [0.0, 1.0, 2.0], name='also_cpu')

    with ops.device('/gpu:0'):
      v_gpu = resource_variable_ops.ResourceVariable(
          [0.0, 1.0, 2.0], name='gpu')

    @function.defun
    def resource_apply_adam():
      training_ops.resource_apply_adam(
          v_cpu.handle,
          v_gpu.handle,
          v_also_cpu.handle,
          1.0,  # beta1_power
          1.0,  # beta2_power
          1.0,  # learning_rate
          1.0,  # beta1
          1.0,  # beta2
          1.0,  # epsilon,
          [1.0, 1.0, 1.0],  # grad
          False)  # use_locking
      return None

    with self.assertRaisesRegexp(
        errors.InvalidArgumentError, 'Could not colocate node with its '
        'resource and reference inputs.*'):
      if not context.executing_eagerly():
        self.evaluate(variables.global_variables_initializer())
      self.evaluate(resource_apply_adam())

  def testFunctionHandlesInputsOnDifferentDevices(self):
    if not context.context().num_gpus():
      self.skipTest('No GPUs found')

    # The Reshape op requires the shape tensor to be placed in host memory.
    reshape = function.defun(array_ops.reshape)
    value = constant_op.constant([1., 2.]).gpu()
    shape = constant_op.constant([2, 1])
    reshaped = reshape(value, shape).cpu()
    self.assertAllEqual(reshaped, [[1], [2]])

  def testFunctionHandlesInputsPlacedOnTheWrongDeviceGracefully(self):
    if not context.context().num_gpus():
      self.skipTest('No GPUs found')

    # The Reshape op requires the shape tensor to be placed in host memory.
    reshape = function.defun(array_ops.reshape)
    value = constant_op.constant([1., 2.])
    shape = constant_op.constant([2, 1]).gpu()
    reshape(value, shape)  # No error is raised

  def testDifferentiableFunctionNoneOutputs(self):

    @function.defun
    def my_function(x):
      return x, None

    def wrapper(x):
      return my_function(x)[0]

    g = backprop.gradients_function(wrapper, [0])(constant_op.constant(0.0))
    self.assertAllEqual(g[0], 1.)

    @function.defun
    def foo(a):
      return None, a * a

    x = constant_op.constant(5.0)
    with backprop.GradientTape() as tp:
      tp.watch(x)
      none, r = foo(x)
    g = tp.gradient(r, x)

    self.assertIs(none, None)
    self.assertAllEqual(r, 25.0)
    self.assertAllEqual(g, 2 * 5.0)

  @test_util.run_in_graph_and_eager_modes
  def testNestedDifferentiableFunction(self):
    @function.defun
    def inner_fn(a, b):
      return a * math_ops.add(a, b)

    @function.defun
    def outer_fn(x):
      return inner_fn(x, 1.0)

    x = constant_op.constant(5.0)
    with backprop.GradientTape() as tp:
      tp.watch(x)
      result = outer_fn(x)
    grad = tp.gradient(result, x)

    self.assertAllEqual(grad, 2 * 5.0 + 1.0)

  @test_util.run_in_graph_and_eager_modes
  def testDeeplyNestedDifferentiableFunction(self):
    @function.defun
    def inner_inner_fn(a, b):
      return math_ops.add(a, b)

    @function.defun
    def inner_fn(a, b):
      return inner_inner_fn(a, b)

    @function.defun
    def middle_fn(a, b):
      return a * inner_fn(a, b)

    @function.defun
    def outer_fn(x):
      return middle_fn(x, 1.0)

    x = constant_op.constant(5.0)
    with backprop.GradientTape() as tp:
      tp.watch(x)
      result = outer_fn(x)
    grad = tp.gradient(result, x)

    self.assertAllEqual(grad, 2 * 5.0 + 1.0)

  @test_util.run_in_graph_and_eager_modes
  def testDeeplyNestedDifferentiableFunctionWithMultipleGradCalls(self):
    @function.defun
    def inner_fn(a, b):
      return math_ops.add(a, b)

    @function.defun
    def middle_fn(a, b):
      return math_ops.mul(a, inner_fn(a, b))

    @function.defun
    def outer_fn(x):
      return middle_fn(x, 3.0)

    x = constant_op.constant(5.0)
    self.assertAllEqual(outer_fn(x), 5.0 * (5.0 + 3.0))

    with backprop.GradientTape() as tp:
      tp.watch(x)
      result = outer_fn(x)
    grad = tp.gradient(result, x)

    self.assertAllEqual(grad, 2 * 5.0 + 3.0)
    self.assertAllEqual(outer_fn(x), 5.0 * (5.0 + 3.0))
    self.assertAllEqual(middle_fn(3.0, x), 3.0 * (3.0 + 5.0))

    with backprop.GradientTape() as tp:
      tp.watch(x)
      result = outer_fn(x)
    grad = tp.gradient(result, x)

    self.assertAllEqual(grad, 2 * 5.0 + 3.0)

    y = constant_op.constant(4.0)
    with backprop.GradientTape() as tp:
      tp.watch(y)
      result = outer_fn(y)
    grad = tp.gradient(result, y)

    self.assertAllEqual(grad, 2 * 4.0 + 3.0)

    with backprop.GradientTape() as tp:
      tp.watch(y)
      result = inner_fn(y, y)
    grad = tp.gradient(result, y)

    self.assertAllEqual(grad, 2.0)

  @test_util.run_in_graph_and_eager_modes
  def testDeeplyNestedDifferentiableFunctionGradientTapeInDefun(self):
    @function.defun
    def inner_inner_fn(a, b):
      return math_ops.add(a, b)

    @function.defun
    def inner_fn(a, b):
      return inner_inner_fn(a, b)

    @function.defun
    def middle_fn(a, b):
      return a * inner_fn(a, b)

    @function.defun
    def outer_fn(x):
      with backprop.GradientTape() as tp:
        tp.watch(x)
        result = middle_fn(x, 1.0)
      grad = tp.gradient(result, x)
      return grad

    x = constant_op.constant(5.0)
    grad = outer_fn(x)
    self.assertAllEqual(grad, 2 * 5.0 + 1.0)

  @test_util.run_in_graph_and_eager_modes
  def testDeeplyNestedDifferentiableFunctionGradientTapeInNestedDefun(self):
    @function.defun
    def inner_inner_fn(a, b):
      return math_ops.add(a, b)

    @function.defun
    def inner_fn(a, b):
      return inner_inner_fn(a, b)

    @function.defun
    def middle_fn(a, b):
      return a * inner_fn(a, b)

    @function.defun
    def almost_outer_fn(x):
      with backprop.GradientTape() as tp:
        tp.watch(x)
        result = middle_fn(x, 1.0)
      grad = tp.gradient(result, x)
      return grad

    @function.defun
    def outer_fn(x):
      return almost_outer_fn(x)

    x = constant_op.constant(5.0)
    grad = outer_fn(x)
    self.assertAllEqual(grad, 2 * 5.0 + 1.0)

  @test_util.run_in_graph_and_eager_modes
  def testDeeplyNestedDifferentiableFunctionGradientTapeInMultNestedDefun(self):
    @function.defun
    def inner_inner_fn(a, b):
      return math_ops.add(a, b)

    @function.defun
    def inner_fn(a, b):
      return inner_inner_fn(a, b)

    @function.defun
    def middle_fn(a, b):
      return a * inner_fn(a, b)

    @function.defun
    def almost_outer_fn(x):
      with backprop.GradientTape() as tp:
        tp.watch(x)
        result = middle_fn(x, 1.0)
      grad = tp.gradient(result, x)
      return grad

    @function.defun
    def outer_fn(x):
      return almost_outer_fn(x)

    @function.defun
    def outer_outer_fn(x):
      return outer_fn(x)

    x = constant_op.constant(5.0)
    grad = outer_outer_fn(x)
    self.assertAllEqual(grad, 2 * 5.0 + 1.0)

  @test_util.run_in_graph_and_eager_modes
  def testDeeplyNestedDifferentiableFunctionTFGradientInDefun(self):
    @function.defun
    def inner_inner_fn(a, b):
      return math_ops.add(a, b)

    @function.defun
    def inner_fn(a, b):
      return inner_inner_fn(a, b)

    @function.defun
    def middle_fn(a, b):
      return a * inner_fn(a, b)

    @function.defun
    def outer_fn(x):
      result = middle_fn(x, 1.0)
      return gradients_impl.gradients(result, [x])[0]

    x = constant_op.constant(5.0)
    grad = outer_fn(x)
    self.assertAllEqual(grad, 2 * 5.0 + 1.0)

  @test_util.run_in_graph_and_eager_modes
  def testDeeplyNestedDifferentiableFunctionTFGradientInNestedDefun(self):
    @function.defun
    def inner_inner_fn(a, b):
      return math_ops.add(a, b)

    @function.defun
    def inner_fn(a, b):
      return inner_inner_fn(a, b)

    @function.defun
    def middle_fn(a, b):
      return a * inner_fn(a, b)

    @function.defun
    def almost_outer_fn(x):
      result = middle_fn(x, 1.0)
      return gradients_impl.gradients(result, [x])[0]

    @function.defun
    def outer_fn(x):
      return almost_outer_fn(x)

    x = constant_op.constant(5.0)
    grad = outer_fn(x)
    self.assertAllEqual(grad, 2 * 5.0 + 1.0)

  @test_util.run_in_graph_and_eager_modes
  def testDeeplyNestedDifferentiableFunctionTFGradientInMultNestedDefun(self):
    @function.defun
    def inner_inner_fn(a, b):
      return math_ops.add(a, b)

    @function.defun
    def inner_fn(a, b):
      return inner_inner_fn(a, b)

    @function.defun
    def middle_fn(a, b):
      return a * inner_fn(a, b)

    @function.defun
    def almost_outer_fn(x):
      result = middle_fn(x, 1.0)
      return gradients_impl.gradients(result, [x])[0]

    @function.defun
    def outer_fn(x):
      return almost_outer_fn(x)

    @function.defun
    def outer_outer_fn(x):
      return outer_fn(x)

    x = constant_op.constant(5.0)
    grad = outer_outer_fn(x)
    self.assertAllEqual(grad, 2 * 5.0 + 1.0)

  def testDeeplyNestedDifferentiableFunctionWithVariable(self):
    var = variables.Variable(constant_op.constant(1.0))

    @function.defun
    def inner_fn(a, b):
      return math_ops.add(a, b)

    @function.defun
    def middle_fn(a, b):
      return a * inner_fn(a, b)

    @function.defun
    def outer_fn(x):
      return middle_fn(x, var)

    x = constant_op.constant(5.0)
    with backprop.GradientTape() as tp:
      tp.watch(x)
      result = outer_fn(x)
    grad = tp.gradient(result, x)

    self.assertAllEqual(grad, 2 * 5.0 + 1.0)

  def testDeeplyNestedDifferentiableFunctionWithVariableMultipleGradCalls(self):
    v = variables.Variable(constant_op.constant(3.0))

    @function.defun
    def inner_fn(a, b):
      return math_ops.add(a, b)

    @function.defun
    def middle_fn(a, b):
      return math_ops.mul(a, inner_fn(a, b))

    @function.defun
    def outer_fn(x):
      return middle_fn(x, v)

    x = constant_op.constant(5.0)
    self.assertAllEqual(outer_fn(x), 5.0 * (5.0 + 3.0))

    with backprop.GradientTape() as tp:
      tp.watch(x)
      result = outer_fn(x)
    grad = tp.gradient(result, x)

    self.assertAllEqual(grad, 2 * 5.0 + 3.0)
    self.assertAllEqual(outer_fn(x), 5.0 * (5.0 + 3.0))
    self.assertAllEqual(middle_fn(v, x), 3.0 * (3.0 + 5.0))

    with backprop.GradientTape() as tp:
      tp.watch(x)
      result = outer_fn(x)
    grad = tp.gradient(result, x)

    self.assertAllEqual(grad, 2 * 5.0 + 3.0)

    y = constant_op.constant(4.0)
    with backprop.GradientTape() as tp:
      tp.watch(y)
      result = outer_fn(y)
    grad = tp.gradient(result, y)

    self.assertAllEqual(grad, 2 * 4.0 + 3.0)

    v.assign(constant_op.constant(1.5))
    with backprop.GradientTape() as tp:
      tp.watch(y)
      result = outer_fn(y)
    grad = tp.gradient(result, y)

    self.assertAllEqual(grad, 2 * 4.0 + 1.5)

    with backprop.GradientTape() as tp:
      tp.watch(y)
      result = inner_fn(y, v)
    grad = tp.gradient(result, y)

    self.assertAllEqual(grad, 1.0)

  def testDeeplyNestedDifferentiableFunctionWithVariableMultipleTFGrads(self):
    with context.graph_mode(), self.cached_session():
      v = resource_variable_ops.ResourceVariable(3.0)
      v.initializer.run()

      @function.defun
      def inner_fn(a, b):
        return math_ops.add(a, b)

      @function.defun
      def middle_fn(a, b):
        return math_ops.mul(a, inner_fn(a, b))

      @function.defun
      def outer_fn(x):
        return middle_fn(x, v)

      x = constant_op.constant(5.0)
      self.assertAllEqual(outer_fn(x).eval(), 5.0 * (5.0 + 3.0))

      grad, = gradients_impl.gradients(outer_fn(x), x)

      self.assertAllEqual(grad, 2 * 5.0 + 3.0)
      self.assertAllEqual(outer_fn(x), 5.0 * (5.0 + 3.0))
      self.assertAllEqual(middle_fn(v, x), 3.0 * (3.0 + 5.0))

      grad, = gradients_impl.gradients(outer_fn(x), x)

      self.assertAllEqual(grad, 2 * 5.0 + 3.0)

      y = constant_op.constant(4.0)
      grad, = gradients_impl.gradients(outer_fn(y), y)
      self.assertAllEqual(grad, 2 * 4.0 + 3.0)

      self.evaluate(v.assign(constant_op.constant(1.5)))
      grad, = gradients_impl.gradients(outer_fn(y), y)

      self.assertAllEqual(grad, 2 * 4.0 + 1.5)

      grad, = gradients_impl.gradients(inner_fn(y, v), y)
      self.assertAllEqual(grad, 1.0)

  def testNestedDifferentiableFunctionNoneOutputs(self):
    @function.defun
    def foo(a, b):
      return None, a * math_ops.add(a, b), None, 2*a

    @function.defun
    def bar(x):
      return foo(x, 1.0)

    x = constant_op.constant(5.0)
    with backprop.GradientTape(persistent=True) as tp:
      tp.watch(x)
      none1, r1, none2, r2 = bar(x)
    g1 = tp.gradient(r1, x)
    g2 = tp.gradient(r2, x)

    self.assertAllEqual(r1, 30.0)
    self.assertAllEqual(r2, 10.0)
    self.assertIs(none1, None)
    self.assertIs(none2, None)
    self.assertAllEqual(g1, 2 * 5.0 + 1.0)
    self.assertAllEqual(g2, 2.0)

  def testNoneOutput(self):

    @function.defun
    def my_function(_):
      return None

    self.assertAllEqual(my_function(1), None)

  def testNestedFunctions(self):
    # TensorFlow function (which is what would be used in TensorFlow graph
    # construction).
    @tf_function.Defun(dtypes.int32, dtypes.int32)
    def add(a, b):
      return math_ops.add(a, b)

    @function.defun
    def add_one(x):
      return add(x, 1)

    self.assertAllEqual(3, add_one(constant_op.constant(2)))

  def testVariableCaptureInNestedFunctions(self):
    v = resource_variable_ops.ResourceVariable(1, dtype=dtypes.int32)

    @function.defun
    def inner_read():
      return v.read_value()

    @function.defun
    def outer():
      return inner_read()

    self.assertEqual(1, int(outer()))

  def testReturnCapturedEagerTensor(self):
    t = constant_op.constant(1)

    @function.defun
    def read():
      return t

    self.assertEqual(1, int(read()))

  def testReturnCapturedGraphTensor(self):
    with context.graph_mode(), self.cached_session():
      t = constant_op.constant(1)

      @function.defun
      def read():
        return t

      self.assertEqual(1, int(self.evaluate(read())))

  def testSequenceInputs(self):
    clip_by_global_norm = function.defun(clip_ops.clip_by_global_norm)
    t_list = [constant_op.constant(1.0), constant_op.constant(2.0)]
    clipped_list, global_norm = clip_by_global_norm(t_list,
                                                    constant_op.constant(.2))
    for t in clipped_list:
      self.assertTrue(isinstance(t, ops.Tensor))
    self.assertTrue(isinstance(global_norm, ops.Tensor))

  def testNestedSequenceInputs(self):

    def my_op(inputs):
      a, b, c = inputs
      e, f = b
      g, h = e
      return [a + a, [tuple([f + f, g + g]), h + h], c + c], a + f + g + h + c

    my_eager_op = function.defun(my_op)
    ret = my_eager_op([
        constant_op.constant(1), [(constant_op.constant(2),
                                   constant_op.constant(3)),
                                  constant_op.constant(4)],
        constant_op.constant(5)
    ])
    self.assertEqual(len(ret), 2)
    self.assertAllEqual(ret[0][0], 2)
    self.assertAllEqual(ret[0][1][0][0], 8)
    self.assertAllEqual(ret[0][1][0][1], 4)
    self.assertTrue(isinstance(ret[0][1][0], tuple))
    self.assertAllEqual(ret[0][1][1], 6)
    self.assertAllEqual(ret[0][2], 10)
    self.assertAllEqual(ret[1], 15)

  def testVariableNamesRespectNameScopesWithDefun(self):
    @function.defun
    def create_variable():
      with ops.name_scope('foo'):
        v = resource_variable_ops.ResourceVariable(0.0, name='bar')
      self.assertEqual(v.name, 'foo/bar:0')

    create_variable()

  def testVariableNamesRespectNameScopesWithDefunInGraph(self):
    with context.graph_mode():
      @function.defun
      def create_variable():
        with ops.name_scope('foo'):
          v = resource_variable_ops.ResourceVariable([1.0, 2.0], name='bar')
        self.assertEqual(v.name, 'foo/bar:0')

      with ops.get_default_graph().as_default():
        create_variable()

  @test_util.run_in_graph_and_eager_modes(assert_no_eager_garbage=True)
  def testLayerInDefun(self):
    conv = convolutional.Conv2D(
        filters=1,
        kernel_size=2,
        kernel_initializer=init_ops.ones_initializer(),
        bias_initializer=init_ops.zeros_initializer())

    @function.defun
    def model(x):
      return conv(x)

    x = array_ops.ones([1, 2, 2, 1])
    y = model(x)

    if not context.executing_eagerly():
      self.evaluate(variables.global_variables_initializer())

    self.assertAllEqual([[[[4.0]]]], self.evaluate(y))

    # Remove reference cycles in model
    test_util.dismantle_polymorphic_function(model)

  @test_util.run_in_graph_and_eager_modes(assert_no_eager_garbage=True)
  def testDefunKerasModelCall(self):
    model = MiniModel()
    model.call = function.defun(model.call)

    x = array_ops.ones([1, 2])
    y = model(x)

    if not context.executing_eagerly():
      self.evaluate(variables.global_variables_initializer())

    self.assertAllEqual([[3.0]], self.evaluate(y))

    # Remove reference cycles in defun.
    test_util.dismantle_polymorphic_function(model.call)
    # Break the reference cycle between the MiniModel and the defun:
    # MiniModel --(through its `call` method)--> PolymorphicFunction
    # PolymorphicFunction --(instancemethod on MiniModel)--> MiniModel
    del model.call

  # Note: The ConfigProto below unfortunately only configures graph
  # construction. Eager's configuration is controlled in `__main__`.
  @test_util.run_in_graph_and_eager_modes(
      config=config_pb2.ConfigProto(device_count={'CPU': 4}))
  def testDeviceAnnotationsRespected(self):

    def multi_device_fn():
      with ops.device('/cpu:0'):
        s0 = test_ops.device_placement_op()
      with ops.device('/cpu:1'):
        s1 = test_ops.device_placement_op()
      with ops.device('/cpu:2'):
        s2 = test_ops.device_placement_op()
      s3 = test_ops.device_placement_op()
      return s0, s1, s2, s3

    defined = function.defun(multi_device_fn)
    outputs = self.evaluate(defined())
    self.assertEqual(len(defined._function_cache), 1)
    self.assertIn(compat.as_bytes('CPU:0'), outputs[0])
    self.assertIn(compat.as_bytes('CPU:1'), outputs[1])
    self.assertIn(compat.as_bytes('CPU:2'), outputs[2])

    with ops.device('/cpu:3'):
      outputs = self.evaluate(defined())
    # All function definitions are agnostic to call site devices.
    self.assertEqual(len(defined._function_cache), 1)
    self.assertIn(compat.as_bytes('CPU:0'), outputs[0])
    self.assertIn(compat.as_bytes('CPU:1'), outputs[1])
    self.assertIn(compat.as_bytes('CPU:2'), outputs[2])
    self.assertIn(compat.as_bytes('CPU:3'), outputs[3])

    with ops.device('/cpu:0'):
      outputs = self.evaluate(defined())
    self.assertEqual(len(defined._function_cache), 1)
    self.assertIn(compat.as_bytes('CPU:0'), outputs[0])
    self.assertIn(compat.as_bytes('CPU:1'), outputs[1])
    self.assertIn(compat.as_bytes('CPU:2'), outputs[2])
    self.assertIn(compat.as_bytes('CPU:0'), outputs[3])

  @test_util.run_in_graph_and_eager_modes(
      config=config_pb2.ConfigProto(device_count={'CPU': 2}))
  def testCallingGraphFunctionOnDifferentDevice(self):

    def func():
      return constant_op.constant(0)

    defined = function.defun(func)
    with ops.device('cpu:0'):
      cpu_graph_function = defined.get_concrete_function()

    with ops.device('cpu:0'):
      self.assertEqual(
          self.evaluate(cpu_graph_function()), self.evaluate(func()))

    with ops.device('cpu:1'):
      self.assertEqual(0., self.evaluate(cpu_graph_function()))

    with ops.device(None):
      self.assertEqual(0., self.evaluate(cpu_graph_function()))

    default_graph_function = defined.get_concrete_function()
    self.assertEqual(
        self.evaluate(default_graph_function()), self.evaluate(func()))

    with ops.device('cpu:1'):
      self.assertEqual(0., self.evaluate(default_graph_function()))

  @test_util.run_in_graph_and_eager_modes
  def testColocateWithRespected(self):
    # TODO(b/113291792): Use multiple CPUs instead of a GPU.
    if not context.context().num_gpus():
      self.skipTest('No GPUs found.')

    with ops.device('cpu:0'):
      x = constant_op.constant(1.0)

    with ops.device('gpu:0'):
      y = constant_op.constant(1.0)

    @function.defun
    def foo():
      return test_ops.device_placement_op()

    with ops.colocate_with(x):
      self.assertIn(compat.as_bytes('CPU:0'), self.evaluate(foo()))

    with ops.colocate_with(y):
      self.assertIn(compat.as_bytes('GPU:0'), self.evaluate(foo()))

  def testVariablesAreTracked(self):
    v = resource_variable_ops.ResourceVariable(1.0)

    def foo(x):
      return v * x

    defined = function.defun(foo)

    x = constant_op.constant([1.0])
    self.assertEqual(1., self.evaluate(defined(x)))
    v.assign(2.)

    x = constant_op.constant([1.0, 2.0])
    self.assertAllEqual([2., 4.], self.evaluate(defined(x)))

  def testCacheObjectHashCollisions(self):

    class Foo(object):

      def __hash__(self):
        return 42

    def func(foo):
      del foo
      return

    defined = function.defun(func)
    defined(Foo())
    self.assertEqual(len(defined._function_cache), 1)

    defined(Foo())
    self.assertEqual(len(defined._function_cache), 2)

  def testCacheTensorShapeDtypeCollision(self):

    def func(t):
      return t + t

    defined = function.defun(func)
    t = constant_op.constant([[1.0]], dtype=dtypes.complex64)
    defined(t)
    self.assertEqual(len(defined._function_cache), 1)

    t = constant_op.constant([1.0], dtype=dtypes.complex128)
    defined(t)
    self.assertEqual(len(defined._function_cache), 2)

  def testCacheTensorUnknownShapesCollision(self):

    def func(t):
      return t + t

    with context.graph_mode(), self.cached_session():
      defined = function.defun(func)

      p = array_ops.placeholder(dtype=dtypes.float32, shape=None)
      defined(p)
      self.assertEqual(len(defined._function_cache), 1)

      p = array_ops.placeholder(dtype=dtypes.float32, shape=[None])
      defined(p)
      self.assertEqual(len(defined._function_cache), 2)

      p = array_ops.placeholder(dtype=dtypes.float32, shape=[None, None])
      defined(p)
      self.assertEqual(len(defined._function_cache), 3)

      t = constant_op.constant(1.0, dtype=dtypes.float32)
      defined(t)
      self.assertEqual(len(defined._function_cache), 4)

  def testPythonFunctionWithDefaultArgs(self):

    def func(foo, bar=1, baz=2):
      del foo
      del bar
      del baz
      return

    defined = function.defun(func)
    defined(0, baz=20)

    def cache_keys():
      """Sanitizes cache keys of non-input metadata."""
      return tuple(key[0] for key in defined._function_cache)

    # `True` corresponds to the fact that we're executing eagerly
    self.assertIn(('URRR', (0, 1, 20)), cache_keys())

    defined(1)  # bar=1, baz=2
    self.assertIn(('URRR', (1, 1, 2)), cache_keys())

    # This matches the previous call.
    defined(foo=1)
    self.assertEqual(len(defined._function_cache), 2)

    defined(1, 2, 3)
    self.assertIn(('URRR', (1, 2, 3)), cache_keys())

    # This matches the previous call.
    defined(1, bar=2, baz=3)
    self.assertEqual(len(defined._function_cache), 3)

    # This matches the previous call.
    defined(1, baz=3, bar=2)
    self.assertEqual(len(defined._function_cache), 3)

  def testFunctoolsPartialUnwrappedCorrectly(self):

    def full_function(a, b, c=3):
      return a, b, c

    partial = functools.partial(full_function, 1, c=3)
    a, b, c = partial(2)

    defined = function.defun(partial)
    func_a, func_b, func_c = defined(2)
    self.assertEqual(func_a.numpy(), a)
    self.assertEqual(func_b.numpy(), b)
    self.assertEqual(func_c.numpy(), c)

  def testInputSignatureWithCompatibleInputs(self):

    def foo(a):
      self.assertEqual(a.shape, (2,))
      return a

    signature = [tensor_spec.TensorSpec(shape=(2,), dtype=dtypes.float32)]
    defined = function.defun(foo, input_signature=signature)
    a = array_ops.ones([2])
    out = defined(a)
    self.assertEqual(len(defined._function_cache), 1)
    self.assertAllEqual(out, a)

    def bar(a):
      self.assertEqual(a._shape_tuple(), (2, None))
      return a

    signature = [tensor_spec.TensorSpec((2, None), dtypes.float32)]
    defined = function.defun(bar, input_signature=signature)
    a = array_ops.ones([2, 1])
    out = defined(a)
    self.assertEqual(len(defined._function_cache), 1)
    self.assertAllEqual(out, a)

    # Changing the second dimension shouldn't create a new function.
    b = array_ops.ones([2, 3])
    out = defined(b)
    self.assertEqual(len(defined._function_cache), 1)
    self.assertAllEqual(out, b)

  def testNestedInputSignatures(self):

    def foo(a, b):
      self.assertEqual(a[0]._shape_tuple(), (2, None))
      self.assertEqual(a[1]._shape_tuple(), (2, None))
      self.assertEqual(b._shape_tuple(), (1,))
      return [a, b]

    signature = [[tensor_spec.TensorSpec((2, None), dtypes.float32)] * 2,
                 tensor_spec.TensorSpec((1,), dtypes.float32)]
    defined = function.defun(foo, input_signature=signature)
    a = array_ops.ones([2, 1])
    b = array_ops.ones([1])
    out = defined([a, a], b)
    self.assertEqual(len(defined._function_cache), 1)
    nest.assert_same_structure(out, [[a, a], b])
    self.assertAllEqual(out[0][0], a)
    self.assertAllEqual(out[0][1], a)
    self.assertAllEqual(out[1], b)

    # Changing the unspecified dimensions shouldn't create a new function.
    a = array_ops.ones([2, 3])
    b = array_ops.ones([2, 5])
    c = array_ops.ones([1])
    out = defined([a, b], c)
    self.assertEqual(len(defined._function_cache), 1)
    nest.assert_same_structure(out, [[a, b], c])
    self.assertAllEqual(out[0][0], a)
    self.assertAllEqual(out[0][1], b)
    self.assertAllEqual(out[1], c)

    def bar(a):
      self.assertEqual(a['a']._shape_tuple(), (2, None))
      self.assertEqual(a['b']._shape_tuple(), (2, None))
      self.assertEqual(a['c']._shape_tuple(), (1,))
      return a

    signature = [{
        'a': tensor_spec.TensorSpec((2, None), dtypes.float32),
        'b': tensor_spec.TensorSpec((2, None), dtypes.float32),
        'c': tensor_spec.TensorSpec((1,), dtypes.float32)
    }]
    a = array_ops.ones([2, 3])
    b = array_ops.ones([1])
    inputs = {'a': a, 'b': a, 'c': b}
    defined = function.defun(bar, input_signature=signature)
    out = defined(inputs)
    nest.assert_same_structure(out, inputs)
    self.assertAllEqual(out['a'], inputs['a'])
    self.assertAllEqual(out['b'], inputs['b'])
    self.assertAllEqual(out['c'], inputs['c'])

  def testInputSignatureMustBeSequenceOfTensorSpecs(self):

    def foo(a, b):
      del a
      del b

    # Signatures must consist exclusively of `TensorSpec` objects.
    signature = [(2, 3), tensor_spec.TensorSpec([2, 3], dtypes.float32)]
    with self.assertRaisesRegexp(TypeError, 'Invalid input_signature.*'):
      function.defun(foo, input_signature=signature)

    # Signatures must be either lists or tuples on their outermost levels.
    signature = {'t1': tensor_spec.TensorSpec([], dtypes.float32)}
    with self.assertRaisesRegexp(TypeError, 'input_signature must be either a '
                                 'tuple or a list.*'):
      function.defun(foo, input_signature=signature)

  def testInputsIncompatibleWithSignatureRaisesError(self):

    def foo(a):
      return a

    signature = [tensor_spec.TensorSpec(shape=(2,), dtype=dtypes.float32)]
    defined = function.defun(foo, input_signature=signature)

    # Invalid shapes.
    with self.assertRaisesRegexp(ValueError, 'Python inputs incompatible.*'):
      defined(array_ops.ones([3]))

    with self.assertRaisesRegexp(ValueError, 'Python inputs incompatible.*'):
      defined(array_ops.ones([2, 1]))

    # Wrong number of arguments.
    with self.assertRaisesRegexp(ValueError,
                                 'Structure of Python function inputs.*'):
      defined(array_ops.ones([2]), array_ops.ones([2]))
    with self.assertRaisesRegexp(ValueError,
                                 'Structure of Python function inputs.*'):
      defined()

  def testInputSignatureForFunctionWithNonTensorInputsNotAllowed(self):

    def foo(a, training=True):
      if training:
        return a
      else:
        return -1.0 * a

    signature = [tensor_spec.TensorSpec([], dtypes.float32)] * 2
    defined = function.defun(foo, input_signature=signature)
    a = constant_op.constant(1.0)
    with self.assertRaisesRegexp(
        ValueError, 'When input_signature is provided, '
        'all inputs to the Python function must be Tensors.'):
      defined(a, training=True)

  def testInputSignatureWithKeywordPositionalArgs(self):

    @function.defun(input_signature=[
        tensor_spec.TensorSpec([], dtypes.float32),
        tensor_spec.TensorSpec([], dtypes.int64)
    ])
    def foo(flt, integer):
      return flt, integer

    flt = constant_op.constant(1.0)
    integer = constant_op.constant(2, dtypes.int64)

    out1, out2 = foo(flt, integer)
    self.assertEqual(len(foo._function_cache), 1)
    self.assertEqual(out1.numpy(), 1.0)
    self.assertEqual(out2.numpy(), 2)

    out1, out2 = foo(flt=flt, integer=integer)
    self.assertEqual(len(foo._function_cache), 1)
    self.assertEqual(out1.numpy(), 1.0)
    self.assertEqual(out2.numpy(), 2)

    out1, out2 = foo(integer=integer, flt=flt)
    self.assertEqual(len(foo._function_cache), 1)
    self.assertEqual(out1.numpy(), 1.0)
    self.assertEqual(out2.numpy(), 2)

    out1, out2 = foo(flt, integer=integer)
    self.assertEqual(len(foo._function_cache), 1)
    self.assertEqual(out1.numpy(), 1.0)
    self.assertEqual(out2.numpy(), 2)

  def testInputSignatureWithKeywordArgsFails(self):

    def foo(a, **kwargs):
      del a
      del kwargs

    with self.assertRaisesRegexp(
        ValueError, 'Cannot define a TensorFlow function from a Python '
        'function with keyword arguments when input_signature.*'):
      function.defun(
          foo,
          input_signature=[
              tensor_spec.TensorSpec([], dtypes.float32),
              tensor_spec.TensorSpec([], dtypes.int64)
          ])

  def testTensorKeywordArguments(self):

    def foo(a, b):
      del a
      return b

    defined = function.defun(foo)
    a = constant_op.constant(2.0)
    b = constant_op.constant([1.0, 2.0])
    one = defined(a, b)
    self.assertEqual(len(defined._function_cache), 1)

    two = defined(a=a, b=b)
    self.assertEqual(len(defined._function_cache), 1)

    three = defined(b=b, a=a)
    self.assertEqual(len(defined._function_cache), 1)

    four = defined(a, b=b)
    self.assertEqual(len(defined._function_cache), 1)

    # The next call corresponds to a new input signature, hence
    # we expect another function to be defined.
    five = defined(b, a)
    self.assertEqual(len(defined._function_cache), 2)

    six = defined(a=b, b=a)
    self.assertEqual(len(defined._function_cache), 2)

    seven = defined(b=a, a=b)
    self.assertEqual(len(defined._function_cache), 2)

    self.assertAllEqual(one, [1.0, 2.0])
    self.assertAllEqual(two, [1.0, 2.0])
    self.assertAllEqual(three, [1.0, 2.0])
    self.assertAllEqual(four, [1.0, 2.0])
    self.assertAllEqual(five, 2.0)
    self.assertAllEqual(six, 2.0)
    self.assertAllEqual(seven, 2.0)

  def testGradientWithKeywordArguments(self):
    matmul = function.defun(math_ops.matmul)

    def sq(x):
      return matmul(a=x, b=x, transpose_a=True)

    t = constant_op.constant([[1.0, 2.0], [3.0, 4.0]])
    grad_t, = backprop.gradients_function(sq, [0])(t)
    self.assertAllEqual(grad_t, [[6, 6], [14, 14]])

    with backprop.GradientTape(persistent=True) as tape:
      tape.watch(t)
      one = matmul(t, b=t, transpose_a=True)
      two = matmul(b=t, a=t, transpose_a=True)
      three = matmul(a=t, b=t, transpose_a=True)

    for output in [one, two, three]:
      self.assertAllEqual(tape.gradient(output, t), [[6, 6], [14, 14]])

  def testGradientInFunctionWithKeywordArguments(self):

    @function.defun
    def f(x):
      return backprop.gradients_function(lambda y: y * y, [0])(x)[0]

    self.assertAllEqual(f(x=constant_op.constant(1.0)), 2.0)

  def testDefuningInstanceMethod(self):

    integer = constant_op.constant(2, dtypes.int64)

    class Foo(object):

      def one(self, tensor):
        return tensor

      @function.defun
      def two(self, tensor, other=integer):
        return self.one(tensor), other

    foo = Foo()
    t = constant_op.constant(1.0)
    one, two = foo.two(t)
    self.assertEqual(one.numpy(), 1.0)
    self.assertEqual(two.numpy(), 2)

  def testDefuningInstanceMethodWithDefaultArgument(self):

    integer = constant_op.constant(2, dtypes.int64)

    class Foo(object):

      @function.defun
      def func(self, other=integer):
        return other

    foo = Foo()
    self.assertEqual(foo.func().numpy(), int(integer))

  def testPythonCallWithSideEffects(self):
    state = []

    @function.defun
    def side_effecting_function():
      state.append(0)

    side_effecting_function()
    self.assertAllEqual(state, [0])

    # The second invocation should call the graph function, which shouldn't
    # trigger the list append.
    side_effecting_function()
    self.assertAllEqual(state, [0])

    # Whereas calling the python function directly should create a side-effect.
    side_effecting_function.python_function()
    self.assertAllEqual(state, [0, 0])

  def testFunctionWithExtraAttributes(self):
    @function.defun_with_attributes(attributes={'experimental_1': 'value1',
                                                'experimental_2': 2})
    def matmul(x, y):
      return math_ops.matmul(x, y)

    def add(x, y):
      return math_ops.add(x, y)
    defun_add = function.defun_with_attributes(
        add, attributes={'experimental_3': True, 'experimental_4': 1.0})

    with context.graph_mode(), self.cached_session():
      with ops.get_default_graph().as_default():
        t = constant_op.constant([[1.0, 2.0], [3.0, 4.0]])
        sq = matmul(t, t)
        double = defun_add(t, t)
        self.assertAllEqual(sq.eval().reshape(-1), [7, 10, 15, 22])
        self.assertAllEqual(double.eval().reshape(-1), [2, 4, 6, 8])

        graph = ops.get_default_graph()
        # pylint: disable=protected-access
        self.assertEqual(len(graph._functions), 2)
        functions = list(graph._functions.values())
        self.assertRegexpMatches(
            functions[0].definition.signature.name, '.*matmul.*')
        attrs = functions[0].definition.attr
        self.assertEqual(len(attrs), 2)
        self.assertEqual(attrs['experimental_1'].s, b'value1')
        self.assertEqual(attrs['experimental_2'].i, 2)

        self.assertRegexpMatches(
            functions[1].definition.signature.name, '.*add.*')
        attrs = functions[1].definition.attr
        self.assertEqual(len(attrs), 2)
        self.assertEqual(attrs['experimental_3'].b, True)
        self.assertEqual(attrs['experimental_4'].f, 1.0)
        # pylint: enable=protected-access

  def testFunctionWithInvalidAttribute(self):
    @function.defun_with_attributes(attributes={'attr1': 'value1'})
    def matmul(x, y):
      return math_ops.matmul(x, y)

    with self.assertRaisesRegexp(ValueError,
                                 '.*Attribute name is not whitelisted.*'):
      with context.graph_mode(), self.cached_session():
        with ops.get_default_graph().as_default():
          t = constant_op.constant([[1.0, 2.0], [3.0, 4.0]])
          matmul(t, t)

    @function.defun_with_attributes(attributes={'experimental_1': ['value1']})
    def add(x, y):
      return math_ops.add(x, y)

    with self.assertRaisesRegexp(ValueError,
                                 '.*Unsupported attribute type.*'):
      with context.graph_mode(), self.cached_session():
        with ops.get_default_graph().as_default():
          t = constant_op.constant([[1.0, 2.0], [3.0, 4.0]])
          add(t, t)

  def testRegisterPolymorphicFunction(self):
    @function.defun
    def add(x, y):
      return math_ops.add(x, y)

    def matmul(x, y):
      return math_ops.matmul(x, y)
    defun_matmul = function.defun(matmul)

    with context.graph_mode(), self.cached_session():
      with ops.get_default_graph().as_default():
        t = constant_op.constant([[1.0, 2.0], [3.0, 4.0]])
        function.register(defun_matmul, t, t)
        function.register(add, t, t)

        graph = ops.get_default_graph()
        # pylint: disable=protected-access
        self.assertEqual(len(graph._functions), 6)
        # two sets of functions, each of them are (inference, forward, backward)
        functions = list(graph._functions.values())
        captured_function_names = [
            f.definition.signature.name for f in functions
        ]
        expected_func_name_regex = [
            '.*inference.*matmul.*',
            '.*forward.*matmul.*',
            '.*inference.*backward.*matmul.*',
            '.*inference.*add.*',
            '.*forward.*add.*',
            '.*inference.*backward.*add.*',
        ]
        for i in range(len(functions)):
          self.assertRegexpMatches(captured_function_names[i],
                                   expected_func_name_regex[i])

        # Check the forward and backward function has the correct attributes.
        self.assertEquals(
            functions[1].definition.attr['backward_function_name'].s,
            functions[2].name)
        self.assertEquals(
            functions[2].definition.attr['forward_function_name'].s,
            functions[1].name)

        self.assertEquals(
            functions[4].definition.attr['backward_function_name'].s,
            functions[5].name)
        self.assertEquals(
            functions[5].definition.attr['forward_function_name'].s,
            functions[4].name)

        sq = defun_matmul(t, t)
        double = add(t, t)
        self.assertAllEqual(sq.eval().reshape(-1), [7, 10, 15, 22])
        self.assertAllEqual(double.eval().reshape(-1), [2, 4, 6, 8])
        # Make sure the pre registered function is used, and no other function
        # is added.
        self.assertEqual(len(graph._functions), 6)
        functions = list(graph._functions.values())
        for i in range(len(functions)):
          self.assertEquals(captured_function_names[i],
                            functions[i].definition.signature.name)

  @parameterized.named_parameters(
      dict(testcase_name='Defun',
           function_decorator=function.defun),
      dict(testcase_name='DefFunction',
           function_decorator=def_function.function))
  def testRegisterConcreteFunction(self, function_decorator):
    @function_decorator
    def py_add(x, y):
      return math_ops.add(x, y)

    py_add(array_ops.ones([]), array_ops.ones([]))
    add = py_add.get_concrete_function(
        tensor_spec.TensorSpec(None, dtypes.float32),
        tensor_spec.TensorSpec(None, dtypes.float32))

    @function_decorator
    def py_composite(x, y):
      return x, add(x, y)

    py_composite(array_ops.ones([]), array_ops.ones([]))
    composite = py_composite.get_concrete_function(
        tensor_spec.TensorSpec(None, dtypes.float32),
        tensor_spec.TensorSpec(None, dtypes.float32))

    with context.graph_mode(), self.cached_session():
      with ops.get_default_graph().as_default():
        t = constant_op.constant([[1.0, 2.0], [3.0, 4.0]])
        function.register_concrete(composite)

        graph = ops.get_default_graph()
        # pylint: disable=protected-access
        self.assertEqual(len(graph._functions), 6)
        # two sets of functions, each of them are (inference, forward, backward)
        functions = list(graph._functions.values())
        captured_function_names = [
            f.definition.signature.name for f in functions
        ]
        expected_func_name_regex = [
            '.*inference.*py_composite.*',
            '.*inference.*py_add.*',
            '.*forward.*py_composite.*',
            '.*forward.*py_add.*',
            '.*inference.*backward.*py_composite.*',
            '.*inference.*backward.*py_add.*',
        ]
        for expected, found in zip(
            expected_func_name_regex,
            captured_function_names):
          self.assertRegexpMatches(found, expected)

        composite_t, composite_double = composite(t, t)
        double = add(t, t)
        self.assertAllEqual([[2, 4], [6, 8]], self.evaluate(double))
        self.assertAllEqual([[2, 4], [6, 8]], self.evaluate(composite_double))
        self.assertAllEqual([[1, 2], [3, 4]], self.evaluate(composite_t))
        # Make sure the pre registered function is used, and no other function
        # is added.
        self.assertEqual(len(graph._functions), 6)

  def testRegisterFunctionWithInputSignature(self):
    def matmul(x, y):
      return math_ops.matmul(x, y)
    defun_matmul = function.defun(
        matmul,
        input_signature=[
            tensor_spec.TensorSpec(shape=(2, 2), dtype=dtypes.float32),
            tensor_spec.TensorSpec(shape=(2, 2), dtype=dtypes.float32)
        ])
    with context.graph_mode(), self.cached_session():
      with ops.get_default_graph().as_default():
        t = constant_op.constant([[1.0, 2.0], [3.0, 4.0]])
        function.register(defun_matmul, t, t)

        graph = ops.get_default_graph()
        # pylint: disable=protected-access
        self.assertEqual(len(graph._functions), 3)

        # Test register function with cache, note inputs are ignored.
        function.register(defun_matmul)
        graph = ops.get_default_graph()
        self.assertEqual(len(graph._functions), 3)

  def testRegisterFunctionWithCache(self):
    def matmul(x, y):
      return math_ops.matmul(x, y)
    defun_matmul = function.defun(matmul)

    with context.graph_mode(), self.cached_session():
      with ops.get_default_graph().as_default():
        t = constant_op.constant([[1.0, 2.0], [3.0, 4.0]])
        t2 = constant_op.constant([[2.0, 3.0], [4.0, 5.0]])
        function.register(defun_matmul, t, t)
        function.register(defun_matmul, t2, t2)

        graph = ops.get_default_graph()
        # Only one function is registered since the input param are in same type
        # pylint: disable=protected-access
        self.assertEqual(len(graph._functions), 3)

  def testCallingFunctionWithDifferentVariables(self):

    @function.defun
    def foo(v):
      v.assign_add(1.0)
      return v.read_value()

    v = resource_variable_ops.ResourceVariable(0.0)
    graph_function = foo.get_concrete_function(v)
    self.assertEqual(len(graph_function.inputs), 1)
    self.assertEqual(len(graph_function.captured_inputs), 0)

    self.assertEqual(float(graph_function(v)), 1.0)
    self.assertEqual(float(graph_function(v)), 2.0)

    w = resource_variable_ops.ResourceVariable(0.0)

    @function.defun
    def bar(v):
      del v
      return constant_op.constant(1.0)

    graph_function = bar.get_concrete_function(v)
    self.assertEqual(float(graph_function(v)), 1.0)
    self.assertEqual(float(graph_function(w)), 1.0)

  def testCallingFunctionWithNonTensorsFails(self):

    @function.defun
    def foo(x):
      return x

    graph_function = foo.get_concrete_function(constant_op.constant(1.0))
    with self.assertRaisesRegexp(ValueError, 'All inputs to `Function`s must '
                                 'be Tensors;.*'):
      graph_function('Not a Tensor.')

  # TODO(scottzhu): Revive the test once the grappler plugin is updated.
  def disabled_testSwapImplementationWithGrapplerPlugin(self):
    rewrites = rewriter_config_pb2.RewriterConfig()
    # function_optimizer has to be turn off, otherwise it will delete the
    # registered function if it does not get called.
    # TODO(scottzhu): Move the ExperimentalImplementationSelector to be called
    # before function_optimizer in future.
    rewrites.function_optimization = rewriter_config_pb2.RewriterConfig.OFF
    customer_optimizer = rewrites.custom_optimizers.add()
    customer_optimizer.name = 'ExperimentalImplementationSelector'
    rewrites.min_graph_nodes = -1
    graph_options = config_pb2.GraphOptions(
        rewrite_options=rewrites, build_cost_model=1)
    config = config_pb2.ConfigProto(graph_options=graph_options)

    with context.graph_mode(), self.cached_session(
        config=config, graph=ops.Graph(), use_gpu=True) as sess:

      @function.defun_with_attributes(
          attributes={
              'experimental_api_implements': 'random_boost',
              'experimental_api_preferred_device': 'CPU'
          })
      def cpu_boost(x):
        return math_ops.add(x, 2.0)

      @function.defun_with_attributes(
          attributes={
              'experimental_api_implements': 'random_boost',
              'experimental_api_preferred_device': 'GPU'
          })
      def gpu_boost(x):
        return math_ops.add(x, 4.0)

      x = constant_op.constant(1.0)

      function.register(cpu_boost, x)
      y = gpu_boost(x)
      y_value = sess.run(y)

      if test.is_gpu_available():
        self.assertEquals(y_value, 5.0)
      else:
        # Grappler fallback to use the CPU impl even called with GPU function.
        self.assertEquals(y_value, 3.0)

  def testDefunFunctionSeparateGraphs(self):
    with context.graph_mode():

      @function.defun
      def add(x):
        return x + 5

      @function.defun
      def maybe_add(x, should_add):
        if should_add:
          return add(x)
        else:
          return x

      with ops.Graph().as_default():
        x = constant_op.constant(11)
        maybe_add(x, True)
        self.assertEqual(len(maybe_add._function_cache), 1)
        self.assertEqual(len(add._function_cache), 1)

        maybe_add(x, False)
        self.assertEqual(len(maybe_add._function_cache), 2)
        self.assertEqual(len(add._function_cache), 1)

      with ops.Graph().as_default():
        x = constant_op.constant(11)
        maybe_add(x, True)
        self.assertEqual(len(maybe_add._function_cache), 3)
        self.assertEqual(len(add._function_cache), 2)

  def testDecoratedMethod(self):
    m = DefunnedMiniModel()
    instance_call_one = m.call(array_ops.ones([1, 2]), training=True)
    instance_call_two = m.call(
        inputs=array_ops.ones([1, 2]), training=True)
    class_call = DefunnedMiniModel.call(m, array_ops.ones([1, 2]),
                                        training=True)
    self.assertAllEqual(instance_call_one, instance_call_two)
    self.assertAllEqual(instance_call_one, class_call)

  def testDecoratedMethodUniquePolymorphicFuncPerInstance(self):
    m = DefunnedMiniModel()
    n = DefunnedMiniModel()

    class_method_one = DefunnedMiniModel.call
    class_method_two = DefunnedMiniModel.call

    m_method_one = m.call
    m_method_two = m.call

    n_method_one = n.call
    n_method_two = n.call

    self.assertEqual(class_method_one, class_method_two)
    self.assertEqual(m_method_one, m_method_two)
    self.assertEqual(n_method_one, n_method_two)
    self.assertNotEqual(m.call, n.call)

  def testDecoratedMethodInspect(self):
    m = DefunnedMiniModel()
    fullargspec = tf_inspect.getfullargspec(m.call)
    self.assertTrue('training' in fullargspec.args)

  def testDecoratedMethodGetConcreteFunction(self):
    m = DefunnedMiniModel()
    instance_call_one = m.call.get_concrete_function(
        array_ops.ones([1, 2]), training=False)
    instance_call_two = m.call.get_concrete_function(
        inputs=array_ops.ones([1, 2]), training=False)
    self.assertAllEqual(instance_call_one(array_ops.ones([1, 2])),
                        instance_call_two(array_ops.ones([1, 2])))

    # Also make sure get_concrete_function works on the class method
    DefunnedMiniModel.call.get_concrete_function(
        m, array_ops.ones([1, 2]), training=False)
    DefunnedMiniModel.call.get_concrete_function(
        m, inputs=array_ops.ones([1, 2]), training=True)


@test_util.with_c_shapes
class AutomaticControlDependenciesTest(test.TestCase):

  def testBasic(self):
    with context.graph_mode(), self.cached_session():
      v = resource_variable_ops.ResourceVariable(1.0)
      variables.global_variables_initializer().run()
      with function.AutomaticControlDependencies() as c:
        v.assign(v + 1)
        v.assign(2 * v)
        val = v.read_value()
        val = c.mark_as_return(val)
      self.assertAllEqual(val.eval(), 4.0)

  def testCondMustRun(self):
    with context.graph_mode(), self.cached_session():
      v = resource_variable_ops.ResourceVariable(1.0)
      variables.global_variables_initializer().run()
      p = array_ops.placeholder(dtype=dtypes.bool)
      with function.AutomaticControlDependencies() as c:

        def true_fn():
          v.assign(v + 1)
          return 0.0

        def false_fn():
          v.assign(v + 4)
          return 1.0

        control_flow_ops.cond(p, true_fn, false_fn)
        val = v.read_value()
        val = c.mark_as_return(val)
      self.assertAllEqual(val.eval(feed_dict={p: False}), 5.0)
      self.assertAllEqual(val.eval(feed_dict={p: True}), 6.0)

  def testCondMustRunSeparateRead(self):
    with context.graph_mode(), self.cached_session():
      v = resource_variable_ops.ResourceVariable(1.0)
      variables.global_variables_initializer().run()
      p = array_ops.placeholder(dtype=dtypes.bool)
      with function.AutomaticControlDependencies() as c:

        def true_fn():
          v.assign(v + 1)
          return 0.0

        def false_fn():
          v.assign(v + 4)
          return 1.0

        control_flow_ops.cond(p, true_fn, false_fn)
        one = constant_op.constant(1.0)
        one = c.mark_as_return(one)
      one.eval(feed_dict={p: False})
      self.assertAllEqual(v.read_value().eval(), 5.0)
      one.eval(feed_dict={p: True})
      self.assertAllEqual(v.read_value().eval(), 6.0)

  def testCondNested(self):
    with context.graph_mode(), self.cached_session():
      v = resource_variable_ops.ResourceVariable(1.0)
      variables.global_variables_initializer().run()
      p = array_ops.placeholder(dtype=dtypes.bool)
      q = array_ops.placeholder(dtype=dtypes.bool)
      with function.AutomaticControlDependencies() as c:

        def true_fn():
          v.assign(v + 1, name='true')
          return 1.0

        def false_fn():

          def inner_true_fn():
            v.assign(v * 2, name='false_true')
            return 2.0

          def inner_false_fn():
            v.assign(v * 3, name='false_false')
            return 3.0

          control_flow_ops.cond(q, inner_true_fn, inner_false_fn)
          return 1.0

        control_flow_ops.cond(p, true_fn, false_fn)
        with ops.name_scope('final'):
          val = v.read_value()
        val = c.mark_as_return(val)
      self.assertAllEqual(val.eval(feed_dict={p: False, q: False}), 3.0)
      self.assertAllEqual(val.eval(feed_dict={p: False, q: True}), 6.0)
      self.assertAllEqual(val.eval(feed_dict={p: True, q: True}), 7.0)
      self.assertAllEqual(val.eval(feed_dict={p: True, q: False}), 8.0)

  def testCondOneBranch(self):
    with context.graph_mode(), self.cached_session():
      v = resource_variable_ops.ResourceVariable(1.0)
      variables.global_variables_initializer().run()
      p = array_ops.placeholder(dtype=dtypes.bool)
      with function.AutomaticControlDependencies() as c:

        def true_fn():
          return 0.0

        def false_fn():
          v.assign(v + 4)
          return 1.0

        control_flow_ops.cond(p, true_fn, false_fn)
        val = v.read_value()
        val = c.mark_as_return(val)
      self.assertAllEqual(val.eval(feed_dict={p: False}), 5.0)
      self.assertAllEqual(val.eval(feed_dict={p: True}), 5.0)

  def testCondOneBranchUpdateBefore(self):
    with context.graph_mode(), self.cached_session():
      v = resource_variable_ops.ResourceVariable(1.0)
      variables.global_variables_initializer().run()
      p = array_ops.placeholder(dtype=dtypes.bool)
      with function.AutomaticControlDependencies() as c:
        v.assign(v * 2)

        def true_fn():
          return 0.0

        def false_fn():
          v.assign(v + 4)
          return 1.0

        control_flow_ops.cond(p, true_fn, false_fn)
        val = v.read_value()
        val = c.mark_as_return(val)
      self.assertAllEqual(val.eval(feed_dict={p: False}), 6.0)
      self.assertAllEqual(val.eval(feed_dict={p: True}), 12.0)

  def testCondOneBranchUpdateAfter(self):
    with context.graph_mode(), self.cached_session():
      v = resource_variable_ops.ResourceVariable(1.0)
      variables.global_variables_initializer().run()
      p = array_ops.placeholder(dtype=dtypes.bool)
      with function.AutomaticControlDependencies() as c:

        def true_fn():
          return 0.0

        def false_fn():
          v.assign(v + 4)
          return 1.0

        control_flow_ops.cond(p, true_fn, false_fn)
        v.assign(v * 2)
        val = v.read_value()
        val = c.mark_as_return(val)
      self.assertAllEqual(val.eval(feed_dict={p: False}), 10.0)
      self.assertAllEqual(val.eval(feed_dict={p: True}), 20.0)

  def testDefunWhileLoopWithCapturedLoopVars(self):
    n = 3
    x = constant_op.constant(list(range(n)))

    @function.defun
    def loop():
      c = lambda i, x: i < n
      b = lambda i, x: (i + 1, x + 1)
      i, out = control_flow_ops.while_loop(c, b, (0, x))
      return i, out

    i, out = loop()
    self.assertEqual(int(i), 3)
    self.assertAllEqual(out, [3, 4, 5])

  def testDecorator(self):
    with context.graph_mode(), self.cached_session():
      v = resource_variable_ops.ResourceVariable(1.0)
      variables.global_variables_initializer().run()

      @function.automatic_control_dependencies
      def f():
        v.assign(v + 1)
        v.assign(2 * v)
        return v.read_value()

      self.assertAllEqual(f().eval(), 4.0)

  def testOptimizerInDefun(self):
    def loss(v):
      return v**2

    optimizer = momentum.MomentumOptimizer(learning_rate=1.0, momentum=1.0)

    @function.defun
    def train():
      self.v = resource_variable_ops.ResourceVariable(1.0)
      grad = backprop.implicit_grad(loss)(self.v)
      optimizer.apply_gradients(grad)
      return self.v.read_value()

    value = train()
    self.assertEqual(value.numpy(), -1.0)

  def testReturningNonTensorRaisesError(self):
    optimizer = momentum.MomentumOptimizer(learning_rate=1.0, momentum=1.0)
    optimizer.apply_gradients = function.defun(optimizer.apply_gradients)
    v = resource_variable_ops.ResourceVariable(1.0)
    grad = backprop.implicit_grad(lambda v: v**2)(v)

    with self.assertRaisesRegexp(TypeError,
                                 '.*must return zero or more Tensors.*'):
      # TODO(akshayka): We might want to allow defun-ing Python functions
      # that return operations (and just execute the op instead of running it).
      optimizer.apply_gradients(grad)

  # TODO(b/111663004): This should work when the outer context is graph
  # building.
  def testOptimizerNonSlotVarsInDefunNoError(self):
    def loss(v):
      return v**2

    optimizer = adam.AdamOptimizer(learning_rate=1.0)

    @function.defun
    def train():
      self.v = resource_variable_ops.ResourceVariable(1.0)
      grad = backprop.implicit_grad(loss)(self.v)
      optimizer.apply_gradients(grad)
      return self.v.read_value()

    train()

  def testOptimizerInDefunWithCapturedVariable(self):
    v = resource_variable_ops.ResourceVariable(1.0)
    def loss():
      return v**2

    optimizer = momentum.MomentumOptimizer(learning_rate=1.0, momentum=1.0)

    @function.defun
    def train():
      grad = backprop.implicit_grad(loss)()
      optimizer.apply_gradients(grad)

    train()
    self.assertEqual(v.numpy(), -1.0)

  def testFunctionModifiesInputList(self):
    # Tests on `list` methods that do in place modification, except `list.sort`
    # since it cannot even be "defunned" in the first place

    def get_list():
      return [constant_op.constant(0.), constant_op.constant(1.)]

    expected_msg = (
        'Function to be traced should not modify structure of input '
        'arguments. Check if your function has list and dictionary '
        'operations that alter input arguments, '
        'such as `list.pop`, `list.append`')

    with self.assertRaisesRegexp(ValueError, expected_msg):

      @function.defun
      def append(l):
        l.append(constant_op.constant(0.))

      append(get_list())

    with self.assertRaisesRegexp(ValueError, expected_msg):

      @function.defun
      def extend(l):
        l.extend([constant_op.constant(0.)])

      extend(get_list())

    with self.assertRaisesRegexp(ValueError, expected_msg):

      @function.defun
      def insert(l):
        l.insert(0, constant_op.constant(0.))

      insert(get_list())

    with self.assertRaisesRegexp(ValueError, expected_msg):

      @function.defun
      def pop(l):
        l.pop()

      pop(get_list())

    with self.assertRaisesRegexp(ValueError, expected_msg):

      @function.defun
      def reverse(l):
        l.reverse()

      reverse(get_list())

    with self.assertRaisesRegexp(ValueError, expected_msg):

      @function.defun
      def remove(l):
        l.remove(l[0])

      remove(get_list())

    # `list.clear` is a method that is in Py3 but not Py2
    if sys.version.startswith('3'):

      with self.assertRaisesRegexp(ValueError, expected_msg):

        @function.defun
        def clear(l):
          l.clear()

        clear(get_list())

    # One last test for keyword arguments
    with self.assertRaisesRegexp(ValueError, expected_msg):

      @function.defun
      def kwdappend(**kwargs):
        l = kwargs['l']
        l.append(constant_op.constant(0.))

      kwdappend(l=get_list())

  def testFunctionModifiesInputDict(self):

    def get_dict():
      return {'t1': constant_op.constant(0.), 't2': constant_op.constant(1.)}

    expected_msg = (
        'Function to be traced should not modify structure of input '
        'arguments. Check if your function has list and dictionary '
        'operations that alter input arguments, '
        'such as `list.pop`, `list.append`')

    with self.assertRaisesRegexp(ValueError, expected_msg):

      @function.defun
      def clear(m):
        m.clear()

      clear(get_dict())

    with self.assertRaisesRegexp(ValueError, expected_msg):

      @function.defun
      def pop(m):
        m.pop('t1')

      pop(get_dict())

    with self.assertRaisesRegexp(ValueError, expected_msg):

      @function.defun
      def popitem(m):
        m.popitem()

      popitem(get_dict())

    with self.assertRaisesRegexp(ValueError, expected_msg):

      @function.defun
      def update(m):
        m.update({'t1': constant_op.constant(3.)})

      update(get_dict())

    with self.assertRaisesRegexp(ValueError, expected_msg):

      @function.defun
      def setdefault(m):
        m.setdefault('t3', constant_op.constant(3.))

      setdefault(get_dict())

  def testFunctionModifiesInputNest(self):
    # Test on functions that modify structure of nested input arguments
    expected_msg = (
        'Function to be traced should not modify structure of input '
        'arguments. Check if your function has list and dictionary '
        'operations that alter input arguments, '
        'such as `list.pop`, `list.append`')

    with self.assertRaisesRegexp(ValueError, expected_msg):

      @function.defun
      def modify(n):
        n[0]['t1'].append(constant_op.constant(1.))

      nested_input = [{
          't1': [constant_op.constant(0.),
                 constant_op.constant(1.)],
      },
                      constant_op.constant(2.)]

      modify(nested_input)

    with self.assertRaisesRegexp(ValueError, expected_msg):

      # The flat list doesn't change whereas the true structure changes
      @function.defun
      def modify_same_flat(n):
        n[0].append(n[1].pop(0))

      nested_input = [[constant_op.constant(0.)],
                      [constant_op.constant(1.),
                       constant_op.constant(2.)]]

      modify_same_flat(nested_input)

  def testDecoratedMethodVariableCleanup(self):
    m = DefunnedMiniModel()
    m(array_ops.ones([1, 2]))
    weak_variables = weakref.WeakSet(m.variables)
    self.assertEqual(2, len(weak_variables))
    del m
    self.assertEqual([], list(weak_variables))


@parameterized.named_parameters(
    dict(testcase_name='Defun', function_decorator=function.defun),
    dict(testcase_name='DefFunction', function_decorator=def_function.function))
class ArgumentNamingTests(test.TestCase, parameterized.TestCase):
  """Tests for recognizable export signatures from concrete functions."""

  def testBasic(self, function_decorator):
    @function_decorator
    def fn(a, b):
      return a + b, a * b
    # Call the function to make def_function happy
    fn(array_ops.ones([]), array_ops.ones([]))

    fn_op = fn.get_concrete_function(
        tensor_spec.TensorSpec(shape=(None,), dtype=dtypes.float32),
        tensor_spec.TensorSpec(shape=(), dtype=dtypes.float32))
    self.assertEqual(
        ['a', 'b'],
        [inp.op.name for inp in fn_op.inputs])
    self.assertEqual(
        [b'a', b'b'],
        [inp.op.get_attr('_user_specified_name') for inp in fn_op.inputs])
    self.assertEqual(2, len(fn_op.graph.structured_outputs))

  def testDictReturned(self, function_decorator):
    @function_decorator
    def fn(x, z=(1., 2.), y=3.):
      z1, z2 = z
      return {'alpha': x + y + z1, 'beta': x * y + z2}
    # Call the function to make def_function happy
    fn(array_ops.ones([]))

    fn_op = fn.get_concrete_function(
        x=tensor_spec.TensorSpec(shape=(None,), dtype=dtypes.float32),
        y=tensor_spec.TensorSpec(shape=(), dtype=dtypes.float32))
    self.assertEqual(
        ['x', 'y'],
        [inp.op.name for inp in fn_op.inputs])
    self.assertEqual(
        [b'x', b'y'],
        [inp.op.get_attr('_user_specified_name') for inp in fn_op.inputs])
    self.assertEqual({'alpha', 'beta'},
                     set(fn_op.graph.structured_outputs.keys()))

    fn_op2 = fn.get_concrete_function(
        z=(tensor_spec.TensorSpec(shape=(None,), dtype=dtypes.float32),
           tensor_spec.TensorSpec(shape=(), dtype=dtypes.float32)),
        y=tensor_spec.TensorSpec(shape=(), dtype=dtypes.float32, name='custom'),
        x=4.)
    self.assertEqual(
        ['z', 'z_1', 'custom'],
        [inp.op.name for inp in fn_op2.inputs])
    self.assertEqual(
        [b'z', b'z', b'custom'],
        [inp.op.get_attr('_user_specified_name') for inp in fn_op2.inputs])

    fn_op3 = fn.get_concrete_function(
        tensor_spec.TensorSpec(shape=(), dtype=dtypes.float32, name='custom'),
        z=(tensor_spec.TensorSpec(shape=(None,), dtype=dtypes.float32),
           tensor_spec.TensorSpec(shape=(), dtype=dtypes.float32)),
        y=tensor_spec.TensorSpec(shape=(), dtype=dtypes.float32, name='custom'))
    self.assertEqual(
        ['custom', 'z', 'z_1', 'custom_1'],
        [inp.op.name for inp in fn_op3.inputs])
    self.assertEqual(
        [b'custom', b'z', b'z', b'custom'],
        [inp.op.get_attr('_user_specified_name') for inp in fn_op3.inputs])

  def testMethod(self, function_decorator):
    class HasMethod(object):

      @function_decorator
      def method(self, x):
        return x

    has_method = HasMethod()
    # Call the function to make def_function happy
    HasMethod.method(has_method, array_ops.ones([]))
    class_op = HasMethod.method.get_concrete_function(
        has_method, tensor_spec.TensorSpec(shape=(), dtype=dtypes.float32))
    self.assertEqual(
        ['x'],
        [inp.op.name for inp in class_op.inputs])
    self.assertEqual(
        [b'x'],
        [inp.op.get_attr('_user_specified_name') for inp in class_op.inputs])
    # Call the function to make def_function happy
    has_method.method(array_ops.ones([]))
    method_op = has_method.method.get_concrete_function(
        tensor_spec.TensorSpec(shape=(), dtype=dtypes.float32))
    self.assertEqual(
        ['x'],
        [inp.op.name for inp in method_op.inputs])
    self.assertEqual(
        [b'x'],
        [inp.op.get_attr('_user_specified_name') for inp in method_op.inputs])
    # TODO(allenl): It should be possible to override names when exporting. Do
    # TensorSpec names need to go in cache keys? Or maybe get_concrete_function
    # should always retrace?
    self.skipTest('Not working')
    method_op = has_method.method.get_concrete_function(
        tensor_spec.TensorSpec(shape=(), dtype=dtypes.float32, name='y'))
    self.assertEqual(
        ['y'],
        [inp.op.name for inp in method_op.inputs])
    self.assertEqual(
        [b'y'],
        [inp.op.get_attr('_user_specified_name') for inp in method_op.inputs])

  def testMethodSignature(self, function_decorator):

    class HasMethod(object):

      @function_decorator(
          input_signature=(tensor_spec.TensorSpec(
              shape=None, dtype=dtypes.float64, name='y'),))
      def method(self, x):
        hash(self)  # No weak proxies passed as `self`
        return x

    has_method = HasMethod()
    # Call the function to make def_function happy
    has_method.method(array_ops.ones([], dtype=dtypes.float64))
    method_op = has_method.method.get_concrete_function()
    self.assertEqual(
        ['y'],
        [inp.op.name for inp in method_op.inputs])
    self.assertEqual(
        [b'y'],
        [inp.op.get_attr('_user_specified_name') for inp in method_op.inputs])
    method_op2 = has_method.method.get_concrete_function()
    self.assertEqual(
        ['y'],
        [inp.op.name for inp in method_op2.inputs])
    self.assertEqual(
        [b'y'],
        [inp.op.get_attr('_user_specified_name') for inp in method_op2.inputs])

  def testVariadic(self, function_decorator):
    @function_decorator
    def variadic_fn(x, *args, **kwargs):
      return x + math_ops.add_n(list(args) + list(kwargs.values()))

    # Call the function to make def_function happy
    variadic_fn(array_ops.ones([]), array_ops.ones([]))
    variadic_op = variadic_fn.get_concrete_function(
        tensor_spec.TensorSpec(shape=(), dtype=dtypes.float32),
        tensor_spec.TensorSpec(shape=None, dtype=dtypes.float32, name='y'),
        tensor_spec.TensorSpec(shape=(), dtype=dtypes.float32),
        tensor_spec.TensorSpec(shape=(), dtype=dtypes.float32),
        z=tensor_spec.TensorSpec(shape=(), dtype=dtypes.float32),
        zz=tensor_spec.TensorSpec(shape=(), dtype=dtypes.float32, name='cust'))
    self.assertEqual(
        ['x', 'y', 'args', 'args_1', 'z', 'cust'],
        [inp.op.name for inp in variadic_op.inputs])
    self.assertEqual(
        [b'x', b'y', b'args', b'args', b'z', b'cust'],
        [inp.op.get_attr('_user_specified_name')
         for inp in variadic_op.inputs])

  def testVariadicInputSignature(self, function_decorator):
    @function_decorator(
        input_signature=(
            tensor_spec.TensorSpec(shape=None, dtype=dtypes.float32),
            tensor_spec.TensorSpec(shape=None, dtype=dtypes.float32, name='y'),
            tensor_spec.TensorSpec(shape=(), dtype=dtypes.float32),
            tensor_spec.TensorSpec(shape=(), dtype=dtypes.float32),
        ))
    def variadic_fn(x, *args):
      return x + math_ops.add_n(list(args))

    # Call the function to make def_function happy
    variadic_fn(array_ops.ones([]), array_ops.ones([]),
                array_ops.ones([]), array_ops.ones([]))
    variadic_op = variadic_fn.get_concrete_function()
    self.assertIn(b'variadic_fn', variadic_op.name)
    self.assertEqual(
        ['x', 'y', 'args', 'args_1'],
        [inp.op.name for inp in variadic_op.inputs])
    self.assertEqual(
        [b'x', b'y', b'args', b'args'],
        [inp.op.get_attr('_user_specified_name')
         for inp in variadic_op.inputs])


if __name__ == '__main__':
  ops.enable_eager_execution(
      config=config_pb2.ConfigProto(device_count={'CPU': 4}))
  test.main()
