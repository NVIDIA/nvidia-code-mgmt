#pragma once

#include <string>

/**
 * @brief
 * @author
 * @since Wed Aug 04 2021
 */
class AssociationInterface
{
  public:
    /**
     * @brief
     * @param
     * @return
     */
    virtual ~AssociationInterface() = default;

    /**
     * @brief
     * @param path
     * @return
     */
    virtual void createActiveAssociation(const std::string& path) = 0;

    /**
     * @brief
     * @param path
     * @return
     */
    virtual void addFunctionalAssociation(const std::string& path) = 0;

    /**
     * @brief
     * @param path
     * @return
     */
    virtual void addUpdateableAssociation(const std::string& path) = 0;

    /**
     * @brief
     * @param path
     * @return
     */
    virtual void removeAssociation(const std::string& path) = 0;
};
