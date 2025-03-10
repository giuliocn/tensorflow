# Copyright 2023 The TensorFlow Authors. All Rights Reserved.
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
"""Fault tolerance tests for distributed save/load."""

import multiprocessing
import time

from absl.testing import parameterized

from tensorflow.python.data.experimental.kernel_tests.service import test_base as data_service_test_base
from tensorflow.python.data.experimental.ops import data_service_ops
from tensorflow.python.data.experimental.ops import distributed_save_op
from tensorflow.python.data.kernel_tests import test_base
from tensorflow.python.data.ops import dataset_ops
from tensorflow.python.data.ops import load_op
from tensorflow.python.framework import combinations
from tensorflow.python.platform import test


class DistributedSaveLoadFtTest(
    data_service_test_base.TestBase, parameterized.TestCase):
  """Fault tolerance tests for distributed save/load."""

  @combinations.generate(
      combinations.times(
          test_base.eager_only_combinations(),
          combinations.combine(
              num_elements=[200],
              num_workers=[1, 3],
              load_repetitions=[1, 3],
              sharding_policy=[
                  data_service_ops.ShardingPolicy.OFF,
                  data_service_ops.ShardingPolicy.DYNAMIC])))
  def test_dispatcher_restart(
      self,
      num_workers: int,
      num_elements: int,
      load_repetitions: int,
      sharding_policy: data_service_ops.ShardingPolicy):
    cluster = data_service_test_base.TestCluster(num_workers=num_workers)
    snapshot_dir = data_service_test_base.TempDir()
    dataset = dataset_ops.Dataset.range(num_elements)
    self.evaluate(
        distributed_save_op.distributed_save(
            dataset, snapshot_dir.full_path, cluster.dispatcher_address()))

    dataset = load_op._load_with_retry(snapshot_dir.full_path)
    dataset = dataset.repeat(load_repetitions)
    dataset = dataset.apply(
        data_service_ops.distribute(
            sharding_policy,
            cluster.dispatcher_address(),
            max_outstanding_requests=1))

    iterator = self.getNext(dataset)
    output = [self.evaluate(iterator())]
    cluster.restart_dispatcher()
    output.extend(self.getIteratorOutput(iterator))

    # For no sharding, dispatcher restarts do not affect data processing
    # happening at the workers.
    if sharding_policy == data_service_ops.ShardingPolicy.OFF:
      expected = list(range(num_elements)) * load_repetitions * num_workers
      self.assertCountEqual(output, expected)

    # Dynamic sharding may lose splits if the dispatcher fails.
    if sharding_policy == data_service_ops.ShardingPolicy.DYNAMIC:
      self.assertNotEmpty(output)
      self.assertContainsSubset(output, range(num_elements))

  @combinations.generate(
      combinations.times(
          test_base.eager_only_combinations(),
          combinations.combine(
              num_elements=[200],
              load_repetitions=[1, 3],
              sharding_policy=[
                  data_service_ops.ShardingPolicy.OFF,
                  data_service_ops.ShardingPolicy.DYNAMIC])))
  def test_dispatcher_and_worker_restart(
      self,
      num_elements: int,
      load_repetitions: int,
      sharding_policy: data_service_ops.ShardingPolicy):
    cluster = data_service_test_base.TestCluster(num_workers=1)
    snapshot_dir = data_service_test_base.TempDir()
    dataset = dataset_ops.Dataset.range(num_elements)
    self.evaluate(
        distributed_save_op.distributed_save(
            dataset, snapshot_dir.full_path, cluster.dispatcher_address()))

    dataset = load_op._load_with_retry(snapshot_dir.full_path)
    dataset = dataset.repeat(load_repetitions)
    dataset = dataset.apply(
        data_service_ops.distribute(
            sharding_policy,
            cluster.dispatcher_address(),
            max_outstanding_requests=1))

    iterator = self.getNext(dataset)
    output = [self.evaluate(iterator())]
    cluster.restart_dispatcher()
    cluster.workers[0].restart()
    output.extend(self.getIteratorOutput(iterator))

    # If the sharding policy is OFF, the restarted worker will produce elements
    # from the beginning of the dataset. The result is a partial range plus
    # `num_elements` repetitions.
    if sharding_policy == data_service_ops.ShardingPolicy.OFF:
      # Search for 0 staring at index 1.
      index_of_second_repetition = output.index(0, 1)
      self.assertCountEqual(
          output[index_of_second_repetition:],
          list(range(num_elements)) * load_repetitions)

    # For dynamic sharding, the first split (and possibly prefetched splits) may
    # be lost. The result is a partial range plus zero or more `num_elements`
    # ranges.
    if sharding_policy == data_service_ops.ShardingPolicy.DYNAMIC:
      try:
        index_of_second_repetition = output.index(0, 1)
        self.assertCountEqual(
            set(output[index_of_second_repetition:]), set(range(num_elements)))
      except ValueError:
        pass

  @combinations.generate(
      combinations.times(
          test_base.eager_only_combinations(),
          combinations.combine(
              load_repetitions=[1, 3],
              sharding_policy=[
                  data_service_ops.ShardingPolicy.OFF,
                  data_service_ops.ShardingPolicy.DYNAMIC])))
  def test_add_worker_midjob(
      self,
      load_repetitions: int,
      sharding_policy: data_service_ops.ShardingPolicy):
    num_elements = 2 * multiprocessing.cpu_count() + 100
    cluster = data_service_test_base.TestCluster(num_workers=1)
    snapshot_dir = data_service_test_base.TempDir()
    dataset = dataset_ops.Dataset.range(num_elements)
    self.evaluate(
        distributed_save_op.distributed_save(
            dataset, snapshot_dir.full_path, cluster.dispatcher_address()))

    dataset = load_op._load_with_retry(snapshot_dir.full_path)
    dataset = dataset.repeat(load_repetitions)
    dataset = dataset.apply(
        data_service_ops.distribute(
            sharding_policy,
            cluster.dispatcher_address(),
            max_outstanding_requests=1))
    expected = list(range(num_elements)) * load_repetitions
    if sharding_policy == data_service_ops.ShardingPolicy.OFF:
      expected *= 2

    # Reads halfway through the dataset.
    iterator = self.getNext(dataset)
    output = [self.evaluate(iterator()) for _ in range(num_elements // 2)]

    # Waits for a new worker to register with the dispatcher.
    cluster.add_worker()
    while cluster.num_registered_workers() < 2:
      time.sleep(10 / 1000)  # 10ms

    output.extend(self.getIteratorOutput(iterator))
    self.assertCountEqual(output, expected)

  @combinations.generate(
      combinations.times(
          test_base.eager_only_combinations(),
          combinations.combine(
              num_workers=[1, 3],
              num_elements=[200],
              load_repetitions=[1, 3],
              sharding_policy=[
                  data_service_ops.ShardingPolicy.OFF,
                  data_service_ops.ShardingPolicy.DYNAMIC])))
  def test_new_dataset_after_restart(
      self,
      num_workers: int,
      num_elements: int,
      load_repetitions: int,
      sharding_policy: data_service_ops.ShardingPolicy):
    cluster = data_service_test_base.TestCluster(num_workers=num_workers)
    snapshot_dir = data_service_test_base.TempDir()
    dataset = dataset_ops.Dataset.range(num_elements)
    self.evaluate(
        distributed_save_op.distributed_save(
            dataset, snapshot_dir.full_path, cluster.dispatcher_address()))

    dataset = load_op._load_with_retry(snapshot_dir.full_path)
    dataset = dataset.repeat(load_repetitions)
    dataset = dataset.apply(
        data_service_ops.distribute(
            sharding_policy, cluster.dispatcher_address()))

    expected = list(range(num_elements)) * load_repetitions
    if sharding_policy == data_service_ops.ShardingPolicy.OFF:
      expected *= num_workers

    # Re-processes the dataset after dispatcher/worker restarts.
    cluster.restart_dispatcher()
    cluster.workers[0].restart()
    self.assertCountEqual(self.getDatasetOutput(dataset), expected)

    cluster.restart_dispatcher()
    cluster.workers[0].restart()
    self.assertCountEqual(self.getDatasetOutput(dataset), expected)


if __name__ == "__main__":
  test.main()
