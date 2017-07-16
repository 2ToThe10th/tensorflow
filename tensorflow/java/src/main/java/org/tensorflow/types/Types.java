package org.tensorflow.types;

import java.util.HashMap;
import java.util.Map;
import org.tensorflow.DataType;

/**
 * Utility class for managing the representation of TensorFlow types as Java
 * types. For each TensorFlow type (e.g., int32), there is a corresponding Java
 * type (e.g., TFInt32) that represents it at compile time and a corresponding
 * class object (e.g., TFInt32.class) that represents it at run time. There is
 * also an enumeration value in DataType that can be used to represent the
 * type, though that should rarely be required.
 */
public class Types {

  private Types() {} // not instantiable

  static final Map<Class<?>, DataType> typeCodes = new HashMap<>();

  /** Returns the DataType value corresponding to a TensorFlow type class. */
  public static DataType dataType(Class<? extends TFType> c) {
    DataType dtype = typeCodes.get(c);
    if (dtype == null) {
      throw new IllegalArgumentException("" + c + " is not a TensorFlow type.");
    }
    return dtype;
  }

  static final Map<Class<?>, Object> scalars = new HashMap<>();

  /** Returns the zero value of type described by {@code c}, or null if
   *  the type (e.g., string) is not numeric and therefore has no zero value.
   */
  public static Object zeroValue(Class<? extends TFType> c) {
    return scalars.get(c);
  }
}
