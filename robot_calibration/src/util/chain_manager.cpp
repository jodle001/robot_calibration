/*
 * Copyright (C) 2022 Michael Ferguson
 * Copyright (C) 2014-2016 Fetch Robotics Inc.
 * Copyright (C) 2013-2014 Unbounded Robotics Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Author: Michael Ferguson

#include <robot_calibration/util/chain_manager.hpp>

static const rclcpp::Logger LOGGER = rclcpp::get_logger("robot_calibration");

namespace robot_calibration
{

ChainManager::ChainManager(rclcpp::Node::SharedPtr node, long int wait_time) :
  state_is_valid_(false)
{
  // Store weak pointer to node
  node_ptr_ = node;

  // We cannot do much without some kinematic chains
  std::vector<std::string> chain_names =
    node->declare_parameter<std::vector<std::string>>("chains", std::vector<std::string>());
  if (chain_names.empty())
  {
    RCLCPP_WARN(LOGGER, "No chains defined.");
    return;
  }

  // Construct each chain to manage
  for (auto name : chain_names)
  {
    std::string topic, group;
    topic = node->declare_parameter<std::string>(name + ".topic", std::string());
    group = node->declare_parameter<std::string>(name + ".planning_group", std::string());

    RCLCPP_INFO(LOGGER, "Creating chain %s on topic %s", name.c_str(), topic.c_str());
    
    std::shared_ptr<ChainController> controller(new ChainController(node, name, topic, group));
    controller->joint_names =
      node->declare_parameter<std::vector<std::string>>(name + ".joints", std::vector<std::string>());

    if (!controller->client.waitForServer(wait_time))
    {
      RCLCPP_WARN(LOGGER, "Failed to connect to %s", topic.c_str());
    }

    if (controller->shouldPlan() && (!move_group_))
    {
      move_group_ = std::make_shared<ActionClient<MoveGroupAction>>();
      move_group_->init(node, "move_action");
      if (!move_group_->waitForServer(wait_time))
      {
        RCLCPP_WARN(LOGGER, "Failed to connect to move_group");
      }
    }

    controllers_.push_back(controller);
  }

  // Parameter to set movement time
  duration_ = node->declare_parameter<double>("duration", 5.0);

  // Parameter to set velocity scaling factor for move_group
  velocity_factor_ = node->declare_parameter<double>("velocity_factor", 1.0);

  // Parameter to limit settling timeout
  // <= 0.0 disables timeout
  settling_timeout_ = node->declare_parameter<double>("settling_timeout", 0.0);

  subscriber_ = node->create_subscription<sensor_msgs::msg::JointState>(
    "/joint_states", 10, std::bind(&ChainManager::stateCallback, this, std::placeholders::_1));
}

void ChainManager::stateCallback(sensor_msgs::msg::JointState::ConstSharedPtr msg)
{
  if (msg->name.size() != msg->position.size())
  {
    RCLCPP_ERROR(LOGGER, "JointState Error: name array is not same size as position array.");
    return;
  }

  if (msg->position.size() != msg->velocity.size())
  {
    RCLCPP_ERROR(LOGGER, "JointState Error: position array is not same size as velocity array.");
    return;
  }

  std::lock_guard<std::mutex> lock(state_mutex_);
  // Update each joint based on message
  for (size_t msg_j = 0; msg_j < msg->name.size(); msg_j++)
  {
    size_t state_j;
    for (state_j = 0; state_j < state_.name.size(); state_j++)
    {
      if (state_.name[state_j] == msg->name[msg_j])
      {
        state_.position[state_j] = msg->position[msg_j];
        state_.velocity[state_j] = msg->velocity[msg_j];
        break;
      }
    }
    if (state_j == state_.name.size())
    {
      // New joint
      state_.name.push_back(msg->name[msg_j]);
      state_.position.push_back(msg->position[msg_j]);
      state_.velocity.push_back(msg->velocity[msg_j]);
    }
  }
  state_is_valid_ = true;
}

bool ChainManager::getState(sensor_msgs::msg::JointState* state)
{
  std::lock_guard<std::mutex> lock(state_mutex_);
  *state = state_;
  return state_is_valid_;
}

trajectory_msgs::msg::JointTrajectoryPoint
ChainManager::makePoint(const sensor_msgs::msg::JointState& state, const std::vector<std::string>& joints)
{
  trajectory_msgs::msg::JointTrajectoryPoint p;
  for (size_t i = 0; i < joints.size(); ++i)
  {
    for (size_t j = 0; j < state.name.size(); ++j)
    {
      if (joints[i] == state.name[j])
      {
        p.positions.push_back(state.position[j]);
        break;
      }
    }
    p.velocities.push_back(0.0);
    p.accelerations.push_back(0.0);
    if (p.velocities.size() != p.positions.size())
    {
      RCLCPP_ERROR(LOGGER, "Bad move to state, missing %s", joints[i].c_str());
      exit(-1);
    }
  }
  return p;
}

bool ChainManager::moveToState(const sensor_msgs::msg::JointState& state)
{
  double max_duration = duration_;

  // Split into different controllers
  for (size_t i = 0; i < controllers_.size(); ++i)
  {
    auto goal = TrajectoryAction::Goal();
    goal.trajectory.joint_names = controllers_[i]->joint_names;

    trajectory_msgs::msg::JointTrajectoryPoint p = makePoint(state, controllers_[i]->joint_names);
    if (controllers_[i]->shouldPlan())
    {
      // Call MoveIt
      auto moveit_goal = MoveGroupAction::Goal();
      moveit_goal.request.group_name = controllers_[i]->chain_planning_group;
      moveit_goal.request.num_planning_attempts = 1;
      moveit_goal.request.allowed_planning_time = 5.0;

      moveit_msgs::msg::Constraints c1;
      c1.joint_constraints.resize(controllers_[i]->joint_names.size());
      for (size_t c = 0; c < controllers_[i]->joint_names.size(); c++)
      {
        c1.joint_constraints[c].joint_name = controllers_[i]->joint_names[c];
        c1.joint_constraints[c].position = p.positions[c];
        c1.joint_constraints[c].tolerance_above = 0.01;
        c1.joint_constraints[c].tolerance_below = 0.01;
        c1.joint_constraints[c].weight = 1.0;
      }
      moveit_goal.request.goal_constraints.push_back(c1);

      // Reduce speed
      moveit_goal.request.max_velocity_scaling_factor = velocity_factor_;

      // All diffs
      moveit_goal.request.start_state.is_diff = true;
      moveit_goal.planning_options.planning_scene_diff.is_diff = true;
      moveit_goal.planning_options.planning_scene_diff.robot_state.is_diff = true;

      // Just make the plan, we will execute it
      moveit_goal.planning_options.plan_only = true;

      move_group_->sendGoal(moveit_goal);
      move_group_->waitForResult(rclcpp::Duration::from_seconds(60.0));
      auto result = move_group_->getResult();
      if (result->error_code.val != moveit_msgs::msg::MoveItErrorCodes::SUCCESS)
      {
        // Unable to plan, return error
        return false;
      }

      goal.trajectory = result->planned_trajectory.joint_trajectory;
      rclcpp::Duration d(goal.trajectory.points[goal.trajectory.points.size()-1].time_from_start);
      max_duration = std::max(max_duration, d.seconds());
    }
    else
    {
      // Go directly to point
      p.time_from_start = rclcpp::Duration::from_seconds(duration_);
      goal.trajectory.points.push_back(p);
    }

    goal.goal_time_tolerance = rclcpp::Duration::from_seconds(1.0);

    // Call actions
    controllers_[i]->client.sendGoal(goal);
  }

  // Wait for results
  for (size_t i = 0; i < controllers_.size(); ++i)
  {
    controllers_[i]->client.waitForResult(rclcpp::Duration::from_seconds(max_duration * 1.5));
    // TODO: catch errors with clients
  }

  return true;
}

bool ChainManager::waitToSettle()
{
  sensor_msgs::msg::JointState state;

  if (controllers_.empty())
  {
    // Nothing to wait for
    return true;
  }

  // Stored as weak pointer, need to grab a real shared pointer
  auto node = node_ptr_.lock();
  if (!node)
  {
    RCLCPP_ERROR(LOGGER, "Unable to get rclcpp::Node lock");
    return false;
  }

  // Reset to invalid so we know state is not stale
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_is_valid_ = false;
  }

  rclcpp::Time start = node->now();
  while (true)
  {
    bool settled = true;

    if (getState(&state))
    {

      // For each joint in state message
      for (size_t j = 0; j < state.name.size(); ++j)
      {
        // Is this joint even a concern?
        if (fabs(state.velocity[j]) < 0.001)
          continue;

        for (size_t i = 0; i < controllers_.size(); ++i)
        {
          for (size_t k = 0; k < controllers_[i]->joint_names.size(); ++k)
          {
            if (controllers_[i]->joint_names[k] == state.name[j])
            {
              settled = false;
              break;
            }
          }
        }

        // If at least one joint is not settled, break out of this for loop
        if (!settled)
        {
          break;
        }
      }
    }
    else
    {
      // State is not yet valid, can't determine if settled
      settled = false;
    }

    // If all joints are settled, break out of while loop
    if (settled)
    {
      break;
    }

    // Exit if timed out
    if (settling_timeout_ > 0.0)
    {
      if ((node->now() - start).seconds() > settling_timeout_)
      {
        // Timed out - return failure
        return false;
      }
    }

    rclcpp::spin_some(node);
  }

  return true;
}

std::vector<std::string> ChainManager::getChains()
{
  std::vector<std::string> chains;
  for (size_t i = 0; i < controllers_.size(); ++i)
  {
    chains.push_back(controllers_[i]->chain_name);
  }
  return chains;
}

std::vector<std::string> ChainManager::getChainJointNames(
  const std::string& chain_name)
{
  for (size_t i = 0; i < controllers_.size(); ++i)
  {
    if (controllers_[i]->chain_name == chain_name)
      return controllers_[i]->joint_names;
  }
  std::vector<std::string> empty;
  return empty;
}

std::string ChainManager::getPlanningGroupName(
  const std::string& chain_name)
{
  for (size_t i = 0; i < controllers_.size(); ++i)
  {
    if (controllers_[i]->chain_name == chain_name)
      return controllers_[i]->chain_planning_group;
  }
  std::vector<std::string> empty;
  return std::string("");
}

}  // namespace robot_calibration
