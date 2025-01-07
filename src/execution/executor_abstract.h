/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "common/common.h"
#include "execution_defs.h"
#include "index/ix.h"
#include "system/sm.h"

class AbstractExecutor {
   public:
    Rid _abstract_rid;

    Context *context_;

    virtual ~AbstractExecutor() = default;

    /**
     * @brief 获取当前记录的列的数据
     * @return 由 ColMeta 组成的表，记录了字段的元数据
     * @note 在火山模型中，这个函数在构建执行器时就会被调用，用于准备输出列的元数据
     *       例如：在 ProjectionExecutor 构造时，需要知道输入列的信息，以确定投影后的列类型
     */
    virtual size_t tupleLen() const { return 0; };

    /**
     * @brief 获取当前记录的列的数据
     * @return 由 ColMeta 组成的表，记录了字段的元数据
     * @note 在火山模型中，这个函数在构建执行器时就会被调用，用于准备输出列的元数据
     *       例如：在 ProjectionExecutor 构造时，需要知道输入列的信息，以确定投影后的列类型
     */
    virtual const std::vector<ColMeta> &cols() const {
        std::vector<ColMeta> *_cols = nullptr;
        return *_cols;
    };

    virtual std::string getType() { return "AbstractExecutor"; };
    /**
     * @brief 初始化并定位到第一条满足条件的记录
     * @note 在火山模型中，这个函数在执行器开始执行时被调用（见 execution_manager.cpp 的 select_from）
     *       它的职责是：
     *       1. 初始化扫描迭代器（如table_iterator）
     *       2. 找到第一条满足条件的记录
     */
    virtual void beginTuple() {};

    /**
     * @brief 移动到下一条满足条件的记录
     * @note 在火山模型中，这个函数在获取下一条记录时被调用
     *       它的职责是：
     *       1. 移动到下一条记录
     *       2. 找到下一条满足条件的记录
     *       3. 如果没有更多满足条件的记录，确保 is_end() 返回true
     */
    virtual void nextTuple() {};

    /**
     * @brief 判断是否已经完成所有记录的扫描
     * @return 如果完成扫描返回 true，否则返回 false
     * @note 在火山模型中，这个函数用于控制扫描循环的终止
     *       见 execution_manager.cpp 中的循环：
     *       for (executor->beginTuple(); !executor->is_end(); executor->nextTuple())
     */
    virtual bool is_end() const { return true; };

    /**
     * @brief 获取当前记录的物理位置信息
     * @return 记录的Rid标识
     * @note 在火山模型中，这个函数用于：
     *       1. 记录的物理定位
     *       2. 用于 Update/Delete 等需要知道记录物理位置的操作
     */
    virtual Rid &rid() = 0;

    /**
     * @brief 获取当前记录
     * @return 当前记录的内容
     * @note 在火山模型中，这个函数在找到满足条件的记录后被调用
     *       它的职责是：
     *       1. 获取当前位置的记录内容
     *       2. 如果没有满足条件的记录，返回 nullptr
     *       注意：这个函数不负责移动位置，移动操作由 nextTuple() 完成
     */
    virtual std::unique_ptr<RmRecord> Next() = 0;

    virtual ColMeta get_col_offset(const TabCol &target) { return ColMeta(); };

    std::vector<ColMeta>::const_iterator get_col(const std::vector<ColMeta> &rec_cols, const TabCol &target) {
        auto pos = std::find_if(rec_cols.begin(), rec_cols.end(), [&](const ColMeta &col) {
            return col.tab_name == target.tab_name && col.name == target.col_name;
        });
        if (pos == rec_cols.end()) {
            throw ColumnNotFoundError(target.tab_name + '.' + target.col_name);
        }
        return pos;
    }
};