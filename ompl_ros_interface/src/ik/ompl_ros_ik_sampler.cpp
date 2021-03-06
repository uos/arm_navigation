/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2008, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of Willow Garage, Inc. nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/

/** \author Sachin Chitta, Ioan Sucan */

#include <ompl_ros_interface/ik/ompl_ros_ik_sampler.h>

namespace ompl_ros_interface
{
bool OmplRosIKSampler::initialize(const ompl::base::StateSpacePtr &state_space,
                                  const std::string &kinematics_solver_name,
                                  const std::string &group_name,
                                  const std::string &end_effector_name,
                                  const planning_environment::CollisionModelsInterface* cmi)
{
  collision_models_interface_ = cmi;
  state_space_ = state_space;
  group_name_ = group_name;
  end_effector_name_ = end_effector_name;

  ros::NodeHandle node_handle("~");

  ROS_DEBUG("Trying to initialize solver %s",kinematics_solver_name.c_str());
  if(!kinematics_loader_.isClassAvailable(kinematics_solver_name))
  {
    ROS_ERROR("pluginlib does not have the class %s",kinematics_solver_name.c_str());
    return false;
  }
  ROS_DEBUG("Found solver %s",kinematics_solver_name.c_str());
  
  try
  {
    kinematics_solver_ = kinematics_loader_.createClassInstance(kinematics_solver_name);
  }
  catch(pluginlib::PluginlibException& ex)    //handle the class failing to load
  {
    ROS_ERROR("The plugin failed to load. Error: %s", ex.what());
    return false;
  }
  ROS_DEBUG("Loaded solver %s",kinematics_solver_name.c_str());

  std::string base_name, tip_name;
  if(!node_handle.hasParam(group_name_+"/root_name"))
  {
    ROS_ERROR_STREAM("Kinematics solver has no root name " << base_name << " in param ns " << node_handle.getNamespace());
    throw new OMPLROSException();
  }
  node_handle.getParam(group_name_+"/root_name",base_name);

  if(!node_handle.hasParam(group_name_+"/tip_name"))
  {
    ROS_ERROR_STREAM("Kinematics solver has no root name " << tip_name << " in param ns " << node_handle.getNamespace());
    throw new OMPLROSException();
  }
  node_handle.getParam(group_name_+"/tip_name",tip_name);

  if(!kinematics_solver_->initialize(group_name,
                                     base_name,
                                     tip_name,
                                     .01)) {
    ROS_ERROR("Could not initialize kinematics solver for group %s",group_name.c_str());
    return false;
  }
  ROS_DEBUG("Initialized solver %s",kinematics_solver_name.c_str());
  scoped_state_.reset(new ompl::base::ScopedState<ompl::base::CompoundStateSpace>(state_space_));
  seed_state_.joint_state.name = kinematics_solver_->getJointNames();
  seed_state_.joint_state.position.resize(kinematics_solver_->getJointNames().size());
  //  arm_navigation_msgs::printJointState(seed_state_.joint_state);
  solution_state_.joint_state.name = kinematics_solver_->getJointNames();
  solution_state_.joint_state.position.resize(kinematics_solver_->getJointNames().size());
  if(!ompl_ros_interface::getRobotStateToOmplStateMapping(seed_state_,*scoped_state_,robot_state_to_ompl_state_mapping_))
  {
    ROS_ERROR("Could not get mapping between robot state and ompl state");
    return false;
  }
  if(!ompl_ros_interface::getOmplStateToRobotStateMapping(*scoped_state_,seed_state_,ompl_state_to_robot_state_mapping_))
  {
    ROS_ERROR("Could not get mapping between ompl state and robot state");
    return false;
  }
  else
  {
    ROS_DEBUG("Real vector index: %d",ompl_state_to_robot_state_mapping_.real_vector_index);
    for(unsigned int i=0; i < ompl_state_to_robot_state_mapping_.real_vector_mapping.size(); i++)
      ROS_DEBUG("mapping: %d %d",i,ompl_state_to_robot_state_mapping_.real_vector_mapping[i]);
  }
  ROS_DEBUG("Initialized Ompl Ros IK Sampler");
  return true;
}  

bool OmplRosIKSampler::configureOnRequest(const arm_navigation_msgs::GetMotionPlan::Request &request,
                                          arm_navigation_msgs::GetMotionPlan::Response &response,
                                          const unsigned int &max_sample_count)
{
  ik_poses_counter_ = 0;
  max_sample_count_ = max_sample_count;
  num_samples_ = 0;
  ik_poses_.clear();
  arm_navigation_msgs::Constraints goal_constraints = request.motion_plan_request.goal_constraints;

  if(!collision_models_interface_->convertConstraintsGivenNewWorldTransform(*collision_models_interface_->getPlanningSceneState(),
                                                                            goal_constraints,
                                                                            kinematics_solver_->getBaseName())) {
    response.error_code.val = response.error_code.FRAME_TRANSFORM_FAILURE;
    return false;
  }
  
  if(!arm_navigation_msgs::constraintsToPoseStampedVector(goal_constraints, ik_poses_))
  {
    ROS_ERROR("Could not get poses from constraints");
    return false;
  }       
  if(ik_poses_.empty())
  {
    ROS_WARN("Could not setup goals for inverse kinematics sampling");
    return false;
  }
  else
    ROS_DEBUG("Setup %d poses for IK sampler",(int)ik_poses_.size());
  for(unsigned int i=0; i < ik_poses_.size(); i++)
  {
    if(ik_poses_[i].header.frame_id != kinematics_solver_->getBaseName())
    {
      ROS_ERROR("Goals for inverse kinematic sampling in %s frame are not in kinematics frame: %s",
                ik_poses_[i].header.frame_id.c_str(),
                kinematics_solver_->getBaseName().c_str());
      return false;
    }
  }
  return true;
}

bool OmplRosIKSampler::sampleGoal(const ompl::base::GoalLazySamples *gls, ompl::base::State *state)
{
  arm_navigation_msgs::RobotState seed_state,solution_state;
  seed_state = seed_state_;
  solution_state = solution_state_; 
  ompl::base::ScopedState<ompl::base::CompoundStateSpace> scoped_state(state_space_);
  //sample a state at random
  scoped_state.random();

  ompl_ros_interface::omplStateToRobotState(scoped_state,
                                            ompl_state_to_robot_state_mapping_,
                                            seed_state);    
  int error_code;
  if(kinematics_solver_->getPositionIK(ik_poses_[ik_poses_counter_].pose,
                                       seed_state.joint_state.position,
                                       solution_state.joint_state.position,
				       error_code))
  {
    // arm_navigation_msgs::printJointState(solution_state.joint_state);
    ompl_ros_interface::robotStateToOmplState(solution_state,
                                              robot_state_to_ompl_state_mapping_,
                                              state);    
    ik_poses_counter_++;
    ik_poses_counter_ = ik_poses_counter_%ik_poses_.size();
    return true;
  }
  return false;
}

bool OmplRosIKSampler::sampleGoals(const ompl::base::GoalLazySamples *gls, ompl::base::State *state)
{
  bool continue_sampling = sampleGoal(gls,state);
  if(continue_sampling)
    num_samples_++;
  //  return continue_sampling && gls->maxSampleCount() < max_sample_count_ && !gls->isAchieved();

  if(num_samples_ > max_sample_count_)
    return false;
  else
    return true;
}

}
