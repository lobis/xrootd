=======================================
:mod:`XRootD.client.env`: Client config
=======================================

.. module:: XRootD.client
   :noindex:

Functions
*********

.. autofunction:: XRootD.client.EnvPutString
.. autofunction:: XRootD.client.EnvGetString
.. autofunction:: XRootD.client.EnvDelString
.. autofunction:: XRootD.client.EnvPutInt
.. autofunction:: XRootD.client.EnvGetInt
.. autofunction:: XRootD.client.EnvDelInt
.. autofunction:: XRootD.client.EnvGetDefault
.. autofunction:: XRootD.client.SetLogLevel
.. autofunction:: XRootD.client.SetLogMask
.. autofunction:: XRootD.client.AuthContext

Classes
*******

.. autoclass:: XRootD.client.EnvContext
   :members:

.. note::

   ``EnvContext`` restores previous values when the context exits, but the
   underlying XRootD client environment is global. Avoid overlapping contexts
   with different values in different threads.
