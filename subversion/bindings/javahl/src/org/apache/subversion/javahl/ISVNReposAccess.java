/**
 * @copyright
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 * @endcopyright
 */

package org.apache.subversion.javahl;

import java.util.Date;
import java.util.Map;

/**
 * This interface is an interface to interact with a remote Subversion
 * repository via the repository access method.
 */
public interface ISVNReposAccess
{
    /**
     * release the native peer (should not depend on finalize)
     */
    void dispose();

    /**
     * @return Version information about the underlying native libraries.
     * @since 1.0
     */
    public Version getVersion();

    /**
     * @param date      The date
     * @return          The latest revision at date
     */
    public long getDatedRevision(Date date)
        throws SubversionException;

    public Map<String, Lock> getLocks(String path, Depth depth)
        throws SubversionException;
}
